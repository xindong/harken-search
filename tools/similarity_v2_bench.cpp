#include "SimilarityQueryV2.h"
#include "SimilarityIndexer.h"
#include "platform/audio_file.h"
#include "nlohmann_json.hpp"
#include "sqlite3.h"
#include <chrono>
#include <filesystem>
#include <unordered_map>
#include <fstream>
#include <iostream>
using namespace HarkenSearch;
int main(int argc,char** argv) {
    if(argc<5) { std::cerr<<"Usage: SimilarityV2Bench library.db model-directory output.json index|diagnose|query [id mode deep]\n";return 2; }
    nlohmann::json report;const std::string action=argv[4];
    try {
        if(action=="index") {
            const auto result=BuildSimilarityIndex(argv[1],argv[2]);
            report={{"ok",result.ok},{"cancelled",result.cancelled},{"saved",result.progress.saved},{"skipped",result.progress.skipped},{"failed",result.progress.failed},{"error",result.error},{"model_error",result.modelError}};
            report["issues"]=nlohmann::json::array();
            for(const auto& issue:result.issues) report["issues"].push_back({{"id",issue.file.id},{"path",issue.file.path},{"reason",int(issue.reason)}});
        } else if(action=="diagnose") {
            SimilarityAnalysisStore store;if(!store.Open(argv[1])) return 2;
            AudioEmbeddingService model; std::string modelError; model.Load(argv[2],modelError);
            std::vector<SimilarityFile> files;store.Files(files);report=nlohmann::json::array();
            for(const auto& file:files) {
                SimilarityAnalysisEntry cached;if(store.Load(file.id,cached) && cached.hasSemantic) continue;
                auto a=AnalyzeSimilarityFile(file.path);SimilarityFileIdentity identity;
                Platform::AudioFile::MonoFloatReader reader;std::vector<float> pcm,block(8192);
                const auto opened=reader.Open(file.path,Platform::AudioFile::LegacyWindowsPathPolicy::FeatureLegacyUtf8VerbatimPrefix);
                for(;;) { const auto read=reader.Read(block.data(),block.size());pcm.insert(pcm.end(),block.begin(),block.begin()+size_t(read.framesRead));if(!read.framesRead || pcm.size()>22050*60) break; }
                auto memory=AnalyzeSimilarityPcm(pcm.data(),pcm.size());
                const auto encoded=model.EncodeFile(file.path,a.analysis.activeStart,a.analysis.activeSeconds);
                report.push_back({{"id",file.id},{"path",file.path},{"file_ok",a.ok},{"file_error",int(a.error)},{"pcm_ok",memory.ok},{"pcm_error",int(memory.error)},{"decoded",pcm.size()},{"opened",int(opened)},{"identity_ok",ReadSimilarityFileIdentity(file.path,identity)}});
                report.back()["model_ok"]=encoded.ok;report.back()["model_error"]=encoded.error;
                report.back()["active_start"]=a.analysis.activeStart;report.back()["active_seconds"]=a.analysis.activeSeconds;
                if(report.size()==5)break;
            }
        } else if(action=="batch" && argc==6) {
            SimilarityAnalysisStore store; if(!store.Open(argv[1])) return 2;
            std::vector<SimilarityFile> files; if(!store.Files(files)) return 2; store.Close();
            std::unordered_map<std::string,SimilarityFile> byPath;
            std::unordered_map<int64_t,AudioFeatureVector> features;
            std::vector<SimilaritySearch::SimilarityFeatureRecord> baselineEntries;
            sqlite3* db=nullptr; if(sqlite3_open_v2(argv[1],&db,SQLITE_OPEN_READONLY,nullptr)!=SQLITE_OK) return 2;
            for(const auto& file:files) {
                byPath[file.path]=file;
                auto extracted=FeatureExtractor::extractFromFileChecked(file.path);
                if(!extracted.ok) throw std::runtime_error("legacy feature extraction failed");
                float lufs=-70,peak=-70; sqlite3_stmt* row=nullptr;
                sqlite3_prepare_v2(db,"SELECT integrated_lufs,true_peak_dbtp FROM audio_files WHERE id=?",-1,&row,nullptr);
                if(row) {
                    sqlite3_bind_int64(row,1,file.id);
                    if(sqlite3_step(row)==SQLITE_ROW && sqlite3_column_type(row,0)!=SQLITE_NULL) { lufs=float(sqlite3_column_double(row,0));peak=float(sqlite3_column_double(row,1)); }
                    sqlite3_finalize(row);
                }
                FeatureExtractor::fillGlobalFeatures(extracted.features,lufs,extracted.features.features[AudioFeatureVector::ENERGY_OFFSET],file.seconds,peak);
                features[file.id]=extracted.features;baselineEntries.push_back({file.id,file.path,extracted.features});
            }
            sqlite3_close(db);SimilaritySearch baseline;baseline.replaceFeatureCache(std::move(baselineEntries));
            std::ifstream input(std::filesystem::u8path(argv[5]));nlohmann::json manifest;input>>manifest;
            SimilarityQueryV2 service(argv[2]);
            report={{"library_count",files.size()},{"quality_labels","not_collected"},{"references",nlohmann::json::array()}};
            for(const auto& ref:manifest.at("references")) {
                const auto found=byPath.find(ref.at("path").get<std::string>());if(found==byPath.end()) throw std::runtime_error("reference not in library");
                const auto& file=found->second;
                nlohmann::json item={{"id",file.id},{"path",file.path},{"name",file.name},{"split",ref.at("split")},{"family",ref.at("family")},{"variants",nlohmann::json::array()}};
                auto began=std::chrono::steady_clock::now();
                const auto old=baseline.findSimilar(features[file.id],file.id,50,SimilarityPreset::Balanced,file.path);
                nlohmann::json oldRows=nlohmann::json::array();
                for(const auto& row:old) oldRows.push_back({{"id",row.fileId},{"path",row.filePath},{"name",byPath.at(row.filePath).name},{"score",row.similarity}});
                item["variants"].push_back({{"mode",-1},{"deep",false},{"query_ms",std::chrono::duration<float,std::milli>(std::chrono::steady_clock::now()-began).count()},{"rows",oldRows}});
                for(int mode=0;mode<2;++mode) for(bool deep:{false,true}) {
                    SimilarityRequest request;request.generation=1;request.analysisVersion=2;request.databasePath=argv[1];request.reference={file.id,file.path,file.name};
                    request.options.mode=mode?SimilarityMode::Substitute:SimilarityMode::Perceptual;request.options.deep=deep;
                    const auto result=service.Execute(request,{});
                    nlohmann::json rows=nlohmann::json::array();
                    for(const auto& row:result.rows)rows.push_back({{"id",row.fileId},{"name",row.fileName},{"path",row.filePath},{"score",row.similarity},{"parts",row.breakdown.parts},{"weights",row.breakdown.weights}});
                    item["variants"].push_back({{"mode",mode},{"deep",deep},{"error",int(result.error)},{"query_ms",result.queryMilliseconds},{"rows",rows}});
                }
                report["references"].push_back(item);
                std::cout<<"reference "<<report["references"].size()<<"/"<<manifest["references"].size()<<std::endl;
            }
        } else if(action=="query" && argc==8) {
            SimilarityAnalysisStore store;if(!store.Open(argv[1]))return 2;
            SimilarityFile file;if(!store.GetFile(std::stoll(argv[5]),file))return 2;store.Close();
            SimilarityRequest request;request.generation=1;request.analysisVersion=2;request.databasePath=argv[1];request.reference={file.id,file.path,file.name};
            request.options.mode=std::stoi(argv[6])?SimilarityMode::Substitute:SimilarityMode::Perceptual;request.options.deep=std::stoi(argv[7])!=0;
            SimilarityQueryV2 service(argv[2]);const auto result=service.Execute(request,{});
            report={{"error",int(result.error)},{"query_ms",result.queryMilliseconds},{"indexed",result.indexed},{"total",result.totalFiles},{"rows",nlohmann::json::array()}};
            for(const auto& row:result.rows)report["rows"].push_back({{"id",row.fileId},{"name",row.fileName},{"path",row.filePath},{"score",row.similarity},{"parts",row.breakdown.parts},{"weights",row.breakdown.weights}});
        } else return 2;
    } catch(const std::exception& error) {report={{"exception",error.what()}};}
    std::ofstream output(argv[3],std::ios::binary);output<<report.dump(2);return output?0:2;
}
