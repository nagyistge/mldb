/* xlsx_importer.cc
   Francois Maillet, 19 janvier 2016

   This file is part of MLDB. Copyright 2016 Datacratic. All rights reserved.
   
   Importer for text files containing a JSON per line
*/

#include "mldb/core/procedure.h"
#include "mldb/core/dataset.h"
#include "mldb/types/value_description.h"
#include "mldb/types/structure_description.h"
#include "mldb/vfs/filter_streams.h"
#include "mldb/types/any_impl.h"
#include "mldb/plugins/for_each_line.h"
#include <boost/lexical_cast.hpp>
#include "mldb/vfs/filter_streams.h"

using namespace std;


namespace Datacratic {
namespace MLDB {


/*****************************************************************************/
/* JSON IMPORTER                                                             */
/*****************************************************************************/

struct JSONImporterConfig : ProcedureConfig {

    JSONImporterConfig() :
          limit(-1),
          offset(0),
          ignoreBadLines(false)
    {}

    Url dataFileUrl;
    PolyConfigT<Dataset> outputDataset;
    
    int64_t limit;
    int64_t offset;
    bool ignoreBadLines;
};

DECLARE_STRUCTURE_DESCRIPTION(JSONImporterConfig);

DEFINE_STRUCTURE_DESCRIPTION(JSONImporterConfig);

JSONImporterConfigDescription::
JSONImporterConfigDescription()
{
    addField("dataFileUrl", &JSONImporterConfig::dataFileUrl,
             "URL to load text file from");
    addField("outputDataset", &JSONImporterConfig::outputDataset,
             "Configuration for output dataset",
             PolyConfigT<Dataset>().withType("sparse.mutable"));
    addField("limit", &JSONImporterConfig::limit,
             "Maximum number of lines to process");
    addField("offset", &JSONImporterConfig::offset,
            "Skip the first n lines.", int64_t(0));
    addField("ignoreBadLines", &JSONImporterConfig::ignoreBadLines,
             "If true, any line causing an error will be skipped. Any line "
             "with an invalid JSON object will cause an error.", false);
    
    addParent<ProcedureConfig>();
}


struct JSONImporter: public Procedure {

    JSONImporter(MldbServer * owner,
                 PolyConfig config_,
                 const std::function<bool (const Json::Value &)> & onProgress)
        : Procedure(owner)
    {
        config = config_.params.convert<JSONImporterConfig>();
    }
    
    JSONImporterConfig config;

    virtual RunOutput run(const ProcedureRunConfig & run,
                          const std::function<bool (const Json::Value &)> & onProgress) const
    {
        auto runProcConf = applyRunConfOverProcConf(config, run);
        
        // Create the output dataset
        std::shared_ptr<Dataset> outputDataset;
 
        if (!runProcConf.outputDataset.type.empty()
            || !runProcConf.outputDataset.id.empty()) {
            outputDataset = obtainDataset(server, runProcConf.outputDataset);
        }

        if(!outputDataset) {
            throw ML::Exception("Unable to obtain output dataset");
        }

        Date zeroTs;

        std::mutex recordMutex;

        std::atomic<int64_t> errors(0);
        std::atomic<int64_t> recordedLines(0);
        auto onLine = [& ](const char * line,
                           size_t lineLength,
                           int64_t blockNumber,
                           int64_t lineNumber)
        {
            if(lineLength == 0)
                return true;

            Json::Reader reader;
            Json::Value root;

            if (!reader.parse(line, line+lineLength, root)) {
                if(!runProcConf.ignoreBadLines)
                    throw ML::Exception(ML::format("Unable to parse line %d to JSON", lineNumber));

                errors++;
                return true;
            }

            if(!root.isObject()) {
                if(!runProcConf.ignoreBadLines)
                    throw ML::Exception(ML::format("JSON at line %d is not an object", lineNumber));

                errors++;
                return true;
            }

            MatrixNamedRow outputRow;
            outputRow.rowName = RowName(ML::format("row%d", lineNumber+1));


            std::function<void (const std::string & id,
                                const Json::Value & val)> emplaceCol = 
                    [&] (const std::string & id,
                         const Json::Value & val)
                {
                    if(val.isNull()) {
                        return;
                    }
                    else if(val.isBool()) {
                        outputRow.columns.emplace_back(ColumnName(id), val.asBool(), zeroTs); 
                    }
                    else if(val.isInt()) {
                        outputRow.columns.emplace_back(ColumnName(id), val.asInt(), zeroTs); 
                    }
                    else if(val.isDouble()) {
                        outputRow.columns.emplace_back(ColumnName(id), val.asDouble(), zeroTs); 
                    }
                    else if(val.isString()) {
                        outputRow.columns.emplace_back(ColumnName(id), val.asString(), zeroTs); 
                    }
                    else if(val.isArray()) {
                        // is it only atomic types?
                        bool onlyAtomic = true;
                        for(int i=0; i<val.size(); i++) {
                            if(val[i].isArray() || val[i].isObject()) {
                                onlyAtomic = false;
                                break;
                            }
                        }

                        if(onlyAtomic) {
                            for(int i=0; i<val.size(); i++) {
                                string key;
                                if(val[i].isString())      key = val[i].asString();
                                else if(val[i].isBool())   key = val[i].asBool() ? "true" : "false";
                                else if(val[i].isDouble()) key = boost::lexical_cast<string>(val[i].asDouble());
                                else if(val[i].isInt())    key = boost::lexical_cast<string>(val[i].asInt());
                                else                       key = val[i].toString();
                                emplaceCol(id + '.' + key, Json::Value(true));
                            }
                        }
                        else {
                            auto str = val.toString();
                            if(str.substr(str.size()-1) == "\n")
                                str = str.substr(0, str.size() -1);
                            outputRow.columns.emplace_back(ColumnName(id), std::move(str), zeroTs); 
                        }
                    }
                    else if(val.isObject()) {
                        for (const std::string & sub_id : val.getMemberNames()) {
                            emplaceCol(id + '.' + sub_id, val[sub_id]);
                        }
                    }
                };

            for (const std::string & id : root.getMemberNames()) {
                emplaceCol(id, root[id]);
            }
            
            recordedLines++;

            std::unique_lock<std::mutex> guard(recordMutex);
            outputDataset->recordRow(outputRow.rowName, outputRow.columns);
            return true;
        };


        ML::filter_istream stream(runProcConf.dataFileUrl.toString());
        forEachLineBlock(stream, onLine, runProcConf.offset, runProcConf.limit);
        outputDataset->commit();

        Json::Value result;
        result["rowCount"] = (int64_t)recordedLines;
        result["numLineErrors"] = (int64_t)errors;
        return RunOutput(result);
    }

    virtual Any getStatus() const
    {
        return Any();
    }
    
    JSONImporterConfig procConfig;
};

static RegisterProcedureType<JSONImporter, JSONImporterConfig>
regJSON(builtinPackage(),
        "import.json",
        "Import a text file with one JSON per line into MLDB",
        "procedures/JSONImporter.md.html");


} // namespace MLDB
} // namespace Datacratic