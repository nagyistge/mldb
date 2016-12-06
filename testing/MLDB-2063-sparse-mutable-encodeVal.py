#
# MLDB-2063-sparse-mutable-encodeVal.py
# Francois Maillet, 2016-11-29
# This file is part of MLDB. Copyright 2016 Datacratic. All rights reserved.
#

mldb = mldb_wrapper.wrap(mldb)  # noqa

class MLDB2063SparseMutableEncodeVal(MldbUnitTest):  # noqa

    @classmethod
    def setUpClass(cls):
        pass

    def test_it(self):
        mldb.post('/v1/procedures', {
                'type': 'transform',
                'params': {
                    'inputData': """
                    SELECT
                        column AS title
                    FROM (SELECT * FROM 
                        row_dataset({
                            "My Value": 1
                    }))
                    """,
                    'outputDataset': "keywords"
                }
            })

        self.assertTableResultEquals(
            mldb.query("select * from keywords"),
            [
                ["_rowName", "title"],
                [       "0",  "My Value" ]
            ]
        )
 
    def test_complex_path(self):
        mldb.post('/v1/procedures', {
            'type': 'transform',
            'params': {
                'inputData': "SELECT CAST ('1.2.3' AS path) AS name",
                'outputDataset': {
                'id': 'sparse',
                    'type': 'sparse.mutable'
                }
            }
        })

        self.assertTableResultEquals(
            mldb.query("SELECT * FROM sparse"),
            [
                ["_rowName", "name"],
                ["result",  "1.2.3" ]
            ]
        )

if __name__ == '__main__':
    mldb.run_tests()
