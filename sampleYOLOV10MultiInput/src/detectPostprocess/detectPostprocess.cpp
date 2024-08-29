/**
* Copyright (c) Huawei Technologies Co., Ltd. 2020-2022. All rights reserved.
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at

* http://www.apache.org/licenses/LICENSE-2.0

* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.

* File sample_process.cpp
* Description: handle acl resource
*/
#include <iostream>
#include "acl/acl.h"
#include "Params.h"
#include "detectPostprocess.h"
#include "AclLiteUtils.h"
#include "AclLiteApp.h"
#include "label.h"

using namespace std;

namespace {
    const uint32_t kSleepTime = 500;
    const double kFountScale = 0.5;
    const cv::Scalar kFountColor(0, 0, 255);
    const uint32_t kLabelOffset = 11;
    const uint32_t kLineSolid = 2;
    const vector <cv::Scalar> kColors{
        cv::Scalar(237, 149, 100), cv::Scalar(0, 215, 255),
        cv::Scalar(50, 205, 50), cv::Scalar(139, 85, 26)};
    typedef struct BoundBox {
        float left;
        float top;
        float right;
        float bottom;
        float score;
        size_t classIndex;
        size_t index;
    } BoundBox;

    bool sortScore(BoundBox box1, BoundBox box2)
    {
        return box1.score > box2.score;
    }
}

DetectPostprocessThread::DetectPostprocessThread(uint32_t modelWidth, uint32_t modelHeight,
    aclrtRunMode& runMode, uint32_t batch)
    :modelWidth_(modelWidth), modelHeight_(modelHeight), runMode_(runMode),
    sendLastBatch_(false), batch_(batch)
{
}

DetectPostprocessThread::~DetectPostprocessThread() {
}

AclLiteError DetectPostprocessThread::Init()
{
    return ACLLITE_OK;
}

AclLiteError DetectPostprocessThread::Process(int msgId, shared_ptr<void> data)
{
    AclLiteError ret = ACLLITE_OK;
    switch (msgId) {
        case MSG_POSTPROC_DETECTDATA:
            InferOutputProcess(static_pointer_cast<DetectDataMsg>(data));
            MsgSend(static_pointer_cast<DetectDataMsg>(data));
            break;
        default:
            ACLLITE_LOG_INFO("Detect PostprocessThread thread ignore msg %d", msgId);
            break;
    }

    return ret;
}

AclLiteError DetectPostprocessThread::InferOutputProcess(shared_ptr<DetectDataMsg> detectDataMsg)
{
    size_t pos = 0;
    for (int n = 0; n < detectDataMsg->decodedImg.size(); n++) {
        void* dataBuffer = CopyDataToHost(detectDataMsg->inferenceOutput[0].data.get() + pos,
            detectDataMsg->inferenceOutput[0].size / batch_, runMode_, MEMORY_NORMAL);
        if (dataBuffer == nullptr) {
            ACLLITE_LOG_ERROR("Copy inference output to host failed");
            return ACLLITE_ERROR_COPY_DATA;
        }
        pos = pos + detectDataMsg->inferenceOutput[0].size / batch_;
        float* detectBuff = static_cast<float*>(dataBuffer);

        // confidence threshold
        float confidenceThreshold = 0.5;

        // total number = (left, top, right, bottom, confidence, class)
        size_t totalNumber = 6;

        // total number of boxs
        size_t modelOutputBoxNum = 300;

        // get srcImage width height
        int srcWidth = detectDataMsg->decodedImg[n].width;
        int srcHeight = detectDataMsg->decodedImg[n].height;

        float scale = min(modelWidth_ * 1.0 / srcWidth * 1.0, modelHeight_ * 1.0 / srcHeight * 1.0);
        uint32_t borderLeftOffset = (modelWidth_ - scale * srcWidth) / 2;
        uint32_t borderTopOffset = (modelHeight_ - scale * srcHeight) / 2;        

        // filter boxes by confidence threshold
        vector <BoundBox> result;
        result.clear();
        size_t leftIndex = 0;
        size_t topIndex = 1;
        size_t rightIndex =2;
        size_t bottomIndex = 3;
        size_t confidenceIndex = 4;
        size_t classIndex = 5;
        for (size_t i = 0; i < modelOutputBoxNum; ++i) {
            float confidence = detectBuff[i * totalNumber + confidenceIndex];
            if (confidence >= confidenceThreshold) {
                BoundBox box;
                box.left = (detectBuff[i * totalNumber + leftIndex] - borderLeftOffset) / scale;
                box.top = (detectBuff[i * totalNumber + topIndex] - borderTopOffset) / scale;
                box.right = (detectBuff[i * totalNumber + rightIndex] - borderLeftOffset) / scale;
                box.bottom = (detectBuff[i * totalNumber + bottomIndex] - borderTopOffset) / scale;
                box.score = confidence;
                box.classIndex = detectBuff[i * totalNumber + classIndex];
                box.index = i;
                result.push_back(box);
            }
        }

        // opencv draw label params
        int half = 2;

        cv::Point leftTopPoint;  // left top
        cv::Point rightBottomPoint;  // right bottom
        string className;  // yolo detect output

        // calculate framenum
        int frameCnt = (detectDataMsg->msgNum) * batch_ + n + 1;

        stringstream sstream;
        sstream.str("");
        sstream << "Channel-" << detectDataMsg->channelId << "-Frame-" << to_string(frameCnt) << "-result: ";
        
        string textHead = "";
        sstream >> textHead;
        string textMid = "[";
        for (size_t i = 0; i < result.size(); ++i) {
            leftTopPoint.x = result[i].left;
            leftTopPoint.y = result[i].top;
            rightBottomPoint.x = result[i].right;
            rightBottomPoint.y = result[i].bottom;
            className = label[result[i].classIndex] + ":" + to_string(result[i].score);
            cv::rectangle(detectDataMsg->frame[n], leftTopPoint, rightBottomPoint, kColors[i % kColors.size()], kLineSolid);
            cv::putText(detectDataMsg->frame[n], className, cv::Point(leftTopPoint.x, leftTopPoint.y + kLabelOffset), cv::FONT_HERSHEY_COMPLEX, kFountScale, kFountColor);
            textMid = textMid + className + " ";            
        }
        string textPrint = textHead + textMid + "]";
        detectDataMsg->textPrint.push_back(textPrint);
        free(detectBuff);
        detectBuff = nullptr;
    }
    return ACLLITE_OK;
}

    
AclLiteError DetectPostprocessThread::MsgSend(shared_ptr<DetectDataMsg> detectDataMsg)
{
    if (!sendLastBatch_) {
        while (1) {
            AclLiteError ret = SendMessage(detectDataMsg->dataOutputThreadId, MSG_OUTPUT_FRAME, detectDataMsg);
            if (ret == ACLLITE_ERROR_ENQUEUE) {
                usleep(kSleepTime);
                continue;
            } else if(ret == ACLLITE_OK) {
                break;
            } else {
                ACLLITE_LOG_ERROR("Send read frame message failed, error %d", ret);
                return ret;
            }
        }
    }
    if (detectDataMsg->isLastFrame && sendLastBatch_) {
        while (1) {
            AclLiteError ret = SendMessage(detectDataMsg->dataOutputThreadId, MSG_ENCODE_FINISH, detectDataMsg);
            if (ret == ACLLITE_ERROR_ENQUEUE) {
                usleep(kSleepTime);
                continue;
            } else if(ret == ACLLITE_OK) {
                break;
            } else {
                ACLLITE_LOG_ERROR("Send read frame message failed, error %d", ret);
                return ret;
            }
        }
    }
    if (detectDataMsg->isLastFrame && !sendLastBatch_) {
        while (1) {
            AclLiteError ret = SendMessage(detectDataMsg->dataOutputThreadId, MSG_ENCODE_FINISH, detectDataMsg);
            if (ret == ACLLITE_ERROR_ENQUEUE) {
                usleep(kSleepTime);
                continue;
            } else if(ret == ACLLITE_OK) {
                break;
            } else {
                ACLLITE_LOG_ERROR("Send read frame message failed, error %d", ret);
                return ret;
            }
        }
        sendLastBatch_ = true;
    }

    return ACLLITE_OK;
}