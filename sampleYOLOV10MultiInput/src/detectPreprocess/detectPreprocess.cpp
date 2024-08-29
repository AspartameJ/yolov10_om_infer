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
#include <sys/timeb.h>
#include "Params.h"
#include "detectPreprocess.h"
#include "AclLiteApp.h"

using namespace std;

namespace {
const uint32_t kSleepTime = 500;
}

DetectPreprocessThread::DetectPreprocessThread(uint32_t modelWidth, uint32_t modelHeight,
    uint32_t batch)
    :modelWidth_(modelWidth), modelHeight_(modelHeight), isReleased(false), batch_(batch)
{
}

DetectPreprocessThread::~DetectPreprocessThread()
{
    if(!isReleased) {
        dvpp_.DestroyResource();
    }
    isReleased = true;
}

AclLiteError DetectPreprocessThread::Init()
{
    AclLiteError aclRet = dvpp_.Init("DVPP_CHNMODE_VPC");
    if (aclRet) {
        ACLLITE_LOG_ERROR("Dvpp init failed, error %d", aclRet);
        return ACLLITE_ERROR;
    }

    return ACLLITE_OK;
}

AclLiteError DetectPreprocessThread::Process(int msgId, shared_ptr<void> data)
{
    switch (msgId) {
        case MSG_PREPROC_DETECTDATA:
            MsgProcess(static_pointer_cast<DetectDataMsg>(data));
            MsgSend(static_pointer_cast<DetectDataMsg>(data));
            break;
        default:
            ACLLITE_LOG_INFO("Detect Preprocess thread ignore msg %d", msgId);
            break;
    }

    return ACLLITE_OK;
}

/**
 * @brief 目标检测预处理线程消息处理函数
 * 
 * 本函数负责对输入的图像数据进行预处理，以满足模型输入的要求。预处理过程包括：
 * 1. 从解码后的图像数据中读取并调整图像大小。
 * 2. 将调整大小后的图像数据拷贝到批次缓冲区中。
 * 
 * @param detectDataMsg 包含解码后图像数据的消息对象
 * @return AclLiteError 返回处理结果，ACL_LITE_OK表示成功，其他值表示错误代码
 */
AclLiteError DetectPreprocessThread::MsgProcess(shared_ptr<DetectDataMsg> detectDataMsg)
{
    AclLiteError ret;
    
    // 计算模型输入缓冲区大小
    uint32_t  modelInputSize = YUV420SP_SIZE(modelWidth_, modelHeight_) * batch_;
    void* buf = nullptr;
    // 分配模型输入缓冲区，优先使用大页内存
    ret = aclrtMalloc(&buf, modelInputSize, ACL_MEM_MALLOC_HUGE_FIRST);
    if ((buf == nullptr) || (ret != ACL_ERROR_NONE)) {
        ACLLITE_LOG_ERROR("Malloc classify inference input buffer failed, "
                          "error %d", ret);
        return ACLLITE_ERROR;
    }
    // 将模型输入缓冲区置零
    uint8_t* batchBuffer = (uint8_t *)buf;
    int32_t setValue = 0;
    aclrtMemset(batchBuffer, modelInputSize, setValue, modelInputSize);
    
    // 遍历解码后的图像数据，进行resize并拷贝到批次缓冲区
    size_t pos = 0;
    for (int i = 0; i < detectDataMsg->decodedImg.size(); i++) {
        ImageData resizedImg, BorderImg;
        // 图像尺寸调整
        float scale = min(modelWidth_ * 1.0 / detectDataMsg->decodedImg[i].width * 1.0, modelHeight_ * 1.0 / detectDataMsg->decodedImg[i].height * 1.0);
        uint32_t resizeWidth = scale * detectDataMsg->decodedImg[i].width;
        uint32_t resizeHeight = scale * detectDataMsg->decodedImg[i].height;
        
        ret = dvpp_.Resize(resizedImg,
            detectDataMsg->decodedImg[i], resizeWidth, resizeHeight);
        if (ret == ACLLITE_ERROR) {
            ACLLITE_LOG_ERROR("Resize image failed");
            return ACLLITE_ERROR;
        }
        ret = dvpp_.Border(BorderImg,
        resizedImg, modelWidth_, modelHeight_);
        if (ret == ACLLITE_ERROR) {
            ACLLITE_LOG_ERROR("Border image failed");
            return ACLLITE_ERROR;
        }
        // 将调整大小后的图像数据拷贝到批次缓冲区
        uint32_t dataSize = YUV420SP_SIZE(modelWidth_, modelHeight_);
        ret = aclrtMemcpy(batchBuffer + pos, dataSize,
        BorderImg.data.get(), BorderImg.size, ACL_MEMCPY_DEVICE_TO_DEVICE);
        pos = pos + dataSize;
    }

    // 更新消息中的模型输入图像数据
    detectDataMsg->modelInputImg.data = SHARED_PTR_DEV_BUF(batchBuffer);
    detectDataMsg->modelInputImg.size = modelInputSize;
    return ACLLITE_OK;
}

AclLiteError DetectPreprocessThread::MsgSend(shared_ptr<DetectDataMsg> detectDataMsg)
{
    while (1) {
        AclLiteError ret = SendMessage(detectDataMsg->detectInferThreadId, MSG_DO_DETECT_INFER, detectDataMsg);
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
    return ACLLITE_OK;
}
