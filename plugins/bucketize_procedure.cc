/**
 * bucketize_procedure.cc
 * Mich, 2015-10-27
 * Copyright (c) 2015 Datacratic Inc. All rights reserved.
 *
 * This file is part of MLDB. Copyright 2015 Datacratic. All rights reserved.
 **/

#include "bucketize_procedure.h"
#include "mldb/server/mldb_server.h"
#include "mldb/sql/sql_expression.h"
#include "mldb/server/dataset_context.h"
#include "mldb/types/basic_value_descriptions.h"
#include "mldb/base/parallel.h"
#include "mldb/server/function_contexts.h"
#include "mldb/server/bound_queries.h"
#include "mldb/sql/table_expression_operations.h"
#include "mldb/sql/join_utils.h"
#include "mldb/sql/execution_pipeline.h"
#include "mldb/arch/backtrace.h"
#include "mldb/types/any_impl.h"
#include "mldb/server/per_thread_accumulator.h"
#include "mldb/types/date.h"
#include "mldb/sql/sql_expression.h"
#include "mldb/plugins/sql_config_validator.h"
#include <memory>

using namespace std;


namespace Datacratic {
namespace MLDB {

BucketizeProcedureConfig::
BucketizeProcedureConfig()
{
    outputDataset.withType("sparse.mutable");
}

DEFINE_STRUCTURE_DESCRIPTION(BucketizeProcedureConfig);

BucketizeProcedureConfigDescription::
BucketizeProcedureConfigDescription()
{
    addField("inputData", &BucketizeProcedureConfig::inputData,
             "An SQL statement to select the input data. The select expression is required "
             "but has no effect.  The order by expression is used to rank the rows prior to "
             "bucketization.");
    addField("outputDataset", &BucketizeProcedureConfig::outputDataset,
             "Output dataset configuration. This may refer either to an "
             "existing dataset, or a fully specified but non-existing dataset "
             "which will be created by the procedure.",
             PolyConfigT<Dataset>().withType("sparse.mutable"));
    addField("percentileBuckets", &BucketizeProcedureConfig::percentileBuckets,
             "Key/ranges of the buckets to create. Buckets ranges can share "
             "start and end values but cannot overlap such that a row can "
             "belong to multiple buckets. \n\n"
             "E.g. `{\"a\": [0, 50], \"b\": [50, 100]}` will give two buckets: "
             "\"a\" with rows where 0% < rank/count <= 50% "
             "and \"b\" with rows where 50% < rank/count <= 100% "
             "where rank is based on the orderBy parameter.");
    addParent<ProcedureConfig>();

    onPostValidate = [&] (BucketizeProcedureConfig * cfg,
                          JsonParsingContext & context)
    {
        vector<pair<float, float>> ranges;
        for (const auto & range: cfg->percentileBuckets) {
            ranges.push_back(range.second);
        }
        auto sorter = [](pair<float, float> a, pair<float, float> b)
        {
            return a.first < b.first;
        };
        sort(ranges.begin(), ranges.end(), sorter);

        auto last = make_pair(-1.0, -1.0);
        for (const auto & range: ranges) {
            if (range.first < 0) {
                throw ML::Exception(
                    "Invalid percentileBucket [%f, %f]: lower bound must be "
                    "greater or equal to 0", range.first, range.second);
            }
            if (range.second > 100) {
                throw ML::Exception(
                    "Invalid percentileBucket [%f, %f]: higher bound must be "
                    "lower or equal to 1", range.first, range.second);
            }
            if (range.first >= range.second) {
                throw ML::Exception(
                    "Invalid percentileBucket [%f, %f]: higher bound must  "
                    "be greater than lower bound", range.first, range.second);
            }
            if (range.first < last.second) {
                throw ML::Exception(
                    "Invalid percentileBucket: [%f, %f] is overlapping with "
                    "[%f, %f]", last.first, last.second, range.first,
                    range.second);
            }
            last = range;
        }
        MustContainFrom<InputQuery>()(cfg->inputData, "bucketize");
    };
}

BucketizeProcedure::
BucketizeProcedure(MldbServer * owner,
                 PolyConfig config,
                 const std::function<bool (const Json::Value &)> & onProgress)
    : Procedure(owner)
{
    procedureConfig = config.params.convert<BucketizeProcedureConfig>();
}

RunOutput
BucketizeProcedure::
run(const ProcedureRunConfig & run,
    const std::function<bool (const Json::Value &)> & onProgress) const
{
    auto runProcConf = applyRunConfOverProcConf(procedureConfig, run);

    SqlExpressionMldbContext context(server);

    auto boundDataset = runProcConf.inputData.stm->from->bind(context);

    SelectExpression select(SelectExpression::parse("1"));
    vector<shared_ptr<SqlExpression> > calc;

    // We calculate an expression with the timestamp of the order by
    // clause.  First, we need to calculate each of the order by clauses
    for (auto & c: runProcConf.inputData.stm->orderBy.clauses) {
        auto whenClause = std::make_shared<FunctionCallExpression>
            ("latest_timestamp", vector<shared_ptr<SqlExpression> >(1, c.first),
             nullptr /* extract */);
        calc.emplace_back(whenClause);
    }

    vector<RowName> orderedRowNames;
    Date globalMaxOrderByTimestamp = Date::negativeInfinity();
    auto getSize = [&] (NamedRowValue & row,
                        const vector<ExpressionValue> & calc)
    {
        for (auto & c: calc) {
            auto ts = c.getAtom().toTimestamp();
            if (ts.isADate()) {
                globalMaxOrderByTimestamp.setMax(c.getAtom().toTimestamp());
            }
        }

        orderedRowNames.emplace_back(row.rowName);
        return true;
    };

    BoundSelectQuery(select,
                     *boundDataset.dataset,
                     boundDataset.asName,
                     runProcConf.inputData.stm->when,
                     *runProcConf.inputData.stm->where,
                     runProcConf.inputData.stm->orderBy,
                     calc)
        .execute(getSize, 
                 runProcConf.inputData.stm->offset, 
                 runProcConf.inputData.stm->limit, 
                 onProgress);

    int64_t rowCount = orderedRowNames.size();
    logger->debug() << "Row count: " << rowCount;

    auto output = createDataset(server, runProcConf.outputDataset,
                                nullptr, true /*overwrite*/);

    typedef tuple<ColumnName, CellValue, Date> Cell;
    PerThreadAccumulator<vector<pair<RowName, vector<Cell> > > > accum;

    for (const auto & mappedRange: runProcConf.percentileBuckets) {
        std::vector<Cell> rowValue;
        rowValue.emplace_back(ColumnName("bucket"),
                              mappedRange.first,
                              globalMaxOrderByTimestamp);
        

        auto applyFct = [&] (int64_t index)
        {
            auto & rows = accum.get();
            rows.reserve(1024);
            rows.emplace_back(orderedRowNames[index], rowValue);

            if (rows.size() >= 1024) {
                output->recordRows(rows);
                rows.clear();
            }
        };
        auto range = mappedRange.second;

        //Make sure that numerical issues dont let 100 percentile go out of bound
        int64_t lowerBound = range.second == 0 ? 0 : int64_t(range.first / 100 * rowCount);
        int64_t higherBound = range.second == 100 ? rowCount : int64_t(range.second / 100 * rowCount);
        
        ExcAssert(higherBound <= rowCount);

        logger->debug() << "Bucket " << mappedRange.first << " from " << lowerBound
                        << " to " << higherBound;

        parallelMap(lowerBound, higherBound, applyFct);
    }

    // record remainder
    accum.forEach([&] (vector<pair<RowName, vector<Cell> > > * rows)
    {
        output->recordRows(*rows);
    });

    output->commit();
    return output->getStatus();
}

Any
BucketizeProcedure::
getStatus() const
{
    return Any();
}

static RegisterProcedureType<BucketizeProcedure, BucketizeProcedureConfig>
regBucketizeProcedure(
    builtinPackage(),
    "bucketize",
    "Assign buckets based on percentile ranges over a sorted dataset",
    "procedures/BucketizeProcedure.md.html",
    nullptr /* static route */,
    { MldbEntity::INTERNAL_ENTITY });
 

} // namespace MLDB
} // namespace Datacratic
