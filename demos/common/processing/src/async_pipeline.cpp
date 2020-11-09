/*
// Copyright (C) 2018-2020 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
*/

#include "async_pipeline.h"
#include <samples/args_helper.hpp>
#include <cldnn/cldnn_config.hpp>
#include <samples/slog.hpp>

using namespace InferenceEngine;
    
PipelineBase::PipelineBase(std::unique_ptr<ModelBase> modelInstance, const CnnConfig& cnnConfig, InferenceEngine::Core& engine) :
    model(std::move(modelInstance)){

    // --------------------------- 1. Load inference engine ------------------------------------------------
    slog::info << "Loading Inference Engine" << slog::endl;

    slog::info << "Device info: " << slog::endl;
    slog::info<< printable(engine.GetVersions(cnnConfig.devices));

    /** Load extensions for the plugin **/
    if (!cnnConfig.cpuExtensionsPath.empty()) {
        // CPU(MKLDNN) extensions are loaded as a shared library and passed as a pointer to base extension
        IExtensionPtr extension_ptr = make_so_pointer<IExtension>(cnnConfig.cpuExtensionsPath.c_str());
        engine.AddExtension(extension_ptr, "CPU");
    }
    if (!cnnConfig.clKernelsConfigPath.empty()) {
        // clDNN Extensions are loaded from an .xml description and OpenCL kernel files
        engine.SetConfig({ {PluginConfigParams::KEY_CONFIG_FILE, cnnConfig.clKernelsConfigPath} }, "GPU");
    }

    // --------------------------- 2. Read IR Generated by ModelOptimizer (.xml and .bin files) ------------
    slog::info << "Loading network files" << slog::endl;
    /** Read network model **/
    InferenceEngine::CNNNetwork cnnNetwork = engine.ReadNetwork(model->getModelFileName());
    /** Set batch size to 1 **/
    slog::info << "Batch size is forced to 1." << slog::endl;

    auto shapes = cnnNetwork.getInputShapes();
    for (auto& shape : shapes) {
        shape.second[0] = 1;
    }
    cnnNetwork.reshape(shapes);

    // -------------------------- Reading all outputs names and customizing I/O blobs (in inherited classes)
    model->prepareInputsOutputs(cnnNetwork);

    // --------------------------- 4. Loading model to the device ------------------------------------------
    slog::info << "Loading model to the device" << slog::endl;
    execNetwork = engine.LoadNetwork(cnnNetwork, cnnConfig.devices, cnnConfig.execNetworkConfig);

    // --------------------------- 5. Create infer requests ------------------------------------------------
    requestsPool.reset(new RequestsPool(execNetwork, cnnConfig.maxAsyncRequests));

    // --------------------------- 6. Call onLoadCompleted to complete initialization of model -------------
    model->onLoadCompleted(&execNetwork, requestsPool.get());
}

PipelineBase::~PipelineBase(){
    waitForTotalCompletion();
}

void PipelineBase::waitForData(){
    std::unique_lock<std::mutex> lock(mtx);

    condVar.wait(lock, [&] {return callbackException != nullptr ||
        requestsPool->isIdleRequestAvailable() ||
        completedInferenceResults.find(outputFrameId) != completedInferenceResults.end();
    });

    if (callbackException)
        std::rethrow_exception(callbackException);
}

int64_t PipelineBase::submitRequest(const InferenceEngine::InferRequest::Ptr& request, const std::shared_ptr<MetaData>& metaData){
    auto frameStartTime = std::chrono::steady_clock::now();
    auto frameID = inputFrameId;

    request->SetCompletionCallback([this,
        frameStartTime,
        frameID,
        request,
        metaData] {
            {
                std::lock_guard<std::mutex> lock(mtx);

                try {
                    InferenceResult result;

                    result.startTime = frameStartTime;

                    result.frameId = frameID;
                    result.metaData = std::move(metaData);
                    for (std::string outName : model->getOutputsNames())
                        result.outputsData.emplace(outName, std::make_shared<TBlob<float>>(*as<TBlob<float>>(request->GetBlob(outName))));

                    completedInferenceResults.emplace(frameID, result);
                    this->requestsPool->setRequestIdle(request);

                    this->onProcessingCompleted(request);
                }
                catch (...) {
                    if (!this->callbackException) {
                        this->callbackException = std::move(std::current_exception());
                    }
                }
            }
            condVar.notify_one();
    });

    inputFrameId++;
    if (inputFrameId < 0)
        inputFrameId = 0;

    request->StartAsync();
    return frameID;
}

int64_t PipelineBase::submitImage(cv::Mat img) {
    auto request = requestsPool->getIdleRequest();
    if (!request)
        return -1;

    std::shared_ptr<MetaData> md;
    model->preprocess(ImageInputData(img), request, md);

    return submitRequest(request, md);
}

std::unique_ptr<ResultBase> PipelineBase::getResult()
{
    auto infResult = PipelineBase::getInferenceResult();
    if (infResult.IsEmpty()) {
        return std::unique_ptr<ResultBase>();
    }

    auto result = model->postprocess(infResult);

    *result = static_cast<ResultBase&>(infResult);
    return result;
}

InferenceResult PipelineBase::getInferenceResult()
{
    InferenceResult retVal;

    {
        std::lock_guard<std::mutex> lock(mtx);

        const auto& it = completedInferenceResults.find(outputFrameId);

        if (it != completedInferenceResults.end())
        {
            retVal = std::move(it->second);
            completedInferenceResults.erase(it);
        }
    }

    if(!retVal.IsEmpty()) {
        outputFrameId = retVal.frameId;
        outputFrameId++;
        if (outputFrameId < 0)
            outputFrameId = 0;

        // Updating performance info
        perfMetrics.update(retVal.startTime);
    }

    return retVal;
}
