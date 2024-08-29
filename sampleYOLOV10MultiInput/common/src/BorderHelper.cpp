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

* File AclLiteImageProc.cpp
* Description: handle dvpp process
*/

#include <iostream>
#include <string.h>
#include "acl/acl.h"
#include "AclLiteUtils.h"
#include "BorderHelper.h"

using namespace std;

BorderHelper::BorderHelper(aclrtStream& stream, acldvppChannelDesc *dvppChannelDesc,
                           uint32_t width, uint32_t height)
    :stream_(stream), vpcOutBufferDev_(nullptr), vpcInputDesc_(nullptr),
    vpcOutputDesc_(nullptr), borderConfig_(nullptr), dvppChannelDesc_(dvppChannelDesc),
    vpcOutBufferSize_(0)
{
    size_.width = width;
    size_.height = height;
}

BorderHelper::~BorderHelper()
{
    DestroyBorderResource();
}

AclLiteError BorderHelper::InitBorderInputDesc(ImageData& inputImage)
{
    uint32_t alignWidth = inputImage.alignWidth;
    uint32_t alignHeight = inputImage.alignHeight;
    if (alignWidth == 0 || alignHeight == 0) {
        ACLLITE_LOG_ERROR("Input image width %d or height %d invalid",
                          inputImage.width, inputImage.height);
        return ACLLITE_ERROR_INVALID_ARGS;
    }

    uint32_t inputBufferSize = 0;
    if (inputImage.format == PIXEL_FORMAT_YUV_SEMIPLANAR_420 || inputImage.format == PIXEL_FORMAT_YVU_SEMIPLANAR_420) {
        inputBufferSize = YUV420SP_SIZE(alignWidth, alignHeight);
        inputImage.width = ALIGN_UP2(inputImage.width);
        inputImage.height = ALIGN_UP2(inputImage.height);
    } else if (inputImage.format == PIXEL_FORMAT_RGB_888 || inputImage.format == PIXEL_FORMAT_BGR_888) {
        inputBufferSize = RGBU8_IMAGE_SIZE(alignWidth, alignHeight);
    } else {
        ACLLITE_LOG_WARNING("Dvpp only support yuv and rgb format.");
    }

    vpcInputDesc_ = acldvppCreatePicDesc();
    if (vpcInputDesc_ == nullptr) {
        ACLLITE_LOG_ERROR("Create dvpp pic desc failed");
        return ACLLITE_ERROR_CREATE_PIC_DESC;
    }

    acldvppSetPicDescData(vpcInputDesc_, inputImage.data.get());
    acldvppSetPicDescFormat(vpcInputDesc_, inputImage.format);
    acldvppSetPicDescWidth(vpcInputDesc_, inputImage.width);
    acldvppSetPicDescHeight(vpcInputDesc_, inputImage.height);
    acldvppSetPicDescWidthStride(vpcInputDesc_, alignWidth);
    acldvppSetPicDescHeightStride(vpcInputDesc_, alignHeight);
    acldvppSetPicDescSize(vpcInputDesc_, inputBufferSize);

    return ACLLITE_OK;
}

AclLiteError BorderHelper::InitBorderOutputDesc()
{
    int borderOutWidth = ALIGN_UP2(size_.width);
    int borderOutHeight = ALIGN_UP2(size_.height);
    int borderOutWidthStride = ALIGN_UP16(borderOutWidth);
    int borderOutHeightStride = ALIGN_UP2(borderOutHeight);
    if (borderOutWidthStride == 0 || borderOutHeightStride == 0) {
        ACLLITE_LOG_ERROR("Align border width(%d) and height(%d) failed",
                          size_.width, size_.height);
        return ACLLITE_ERROR_INVALID_ARGS;
    }

    vpcOutBufferSize_ = YUV420SP_SIZE(borderOutWidthStride, borderOutHeightStride);
    aclError aclRet = acldvppMalloc(&vpcOutBufferDev_, vpcOutBufferSize_);
    if (aclRet != ACL_SUCCESS) {
        ACLLITE_LOG_ERROR("Dvpp border malloc output buffer failed, "
                          "size %d, error %d", vpcOutBufferSize_, aclRet);
        return ACLLITE_ERROR_MALLOC_DVPP;
    }

    vpcOutputDesc_ = acldvppCreatePicDesc();
    if (vpcOutputDesc_ == nullptr) {
        ACLLITE_LOG_ERROR("acldvppCreatePicDesc vpcOutputDesc_ failed");
        return ACLLITE_ERROR_CREATE_PIC_DESC;
    }

    acldvppSetPicDescData(vpcOutputDesc_, vpcOutBufferDev_);
    acldvppSetPicDescFormat(vpcOutputDesc_, PIXEL_FORMAT_YUV_SEMIPLANAR_420);
    acldvppSetPicDescWidth(vpcOutputDesc_, borderOutWidth);
    acldvppSetPicDescHeight(vpcOutputDesc_, borderOutHeight);
    acldvppSetPicDescWidthStride(vpcOutputDesc_, borderOutWidthStride);
    acldvppSetPicDescHeightStride(vpcOutputDesc_, borderOutHeightStride);
    acldvppSetPicDescSize(vpcOutputDesc_, vpcOutBufferSize_);

    return ACLLITE_OK;
}

AclLiteError BorderHelper::InitBorderResource(ImageData& inputImage)
{
    borderConfig_ = acldvppCreateBorderConfig();
    if (borderConfig_ == nullptr) {
        ACLLITE_LOG_ERROR("Dvpp border init failed for create config failed");
        return ACLLITE_ERROR_CREATE_RESIZE_CONFIG;
    }
    
    AclLiteError ret = InitBorderInputDesc(inputImage);
    if (ret != ACLLITE_OK) {
        ACLLITE_LOG_ERROR("InitBorderInputDesc failed");
        return ret;
    }
    
    ret = InitBorderOutputDesc();
    if (ret != ACLLITE_OK) {
        ACLLITE_LOG_ERROR("InitBorderOutputDesc failed");
        return ret;
    }

    return ACLLITE_OK;
}

AclLiteError BorderHelper::Process(ImageData& borderImage, ImageData& srcImage)
{
    AclLiteError atlRet = InitBorderResource(srcImage);
    if (atlRet != ACLLITE_OK) {
        ACLLITE_LOG_ERROR("Dvpp border failed for init error");
        return atlRet;
    }
    // border config
    acldvppBorderType borderType = BORDER_CONSTANT;
    vector<double> borderValue = {114, 128, 128};

    uint32_t totalWidthDiff = size_.width - srcImage.width;
    uint32_t totalHeightDiff = size_.height - srcImage.height;
    uint32_t borderLeftOffset = totalWidthDiff / 2;
    uint32_t borderTopOffset = totalHeightDiff / 2;
    uint32_t borderRightOffset = totalWidthDiff - borderLeftOffset;
    uint32_t borderBottomOffset = totalHeightDiff - borderTopOffset;


    aclError aclRet = acldvppSetBorderConfigBorderType(borderConfig_, borderType);
    if (atlRet != ACLLITE_OK) {
        ACLLITE_LOG_ERROR("[ERROR] acldvppSetBorderConfigBorderType failed. ERROR: %d", aclRet);
        return atlRet;
    }
    aclRet = acldvppSetBorderConfigLeft(borderConfig_, borderLeftOffset);
    if (atlRet != ACLLITE_OK) {
        ACLLITE_LOG_ERROR("[ERROR] acldvppSetBorderConfigLeft failed. ERROR: %d", aclRet);
        return atlRet;
    }
    aclRet = acldvppSetBorderConfigTop(borderConfig_, borderTopOffset);
    if (atlRet != ACLLITE_OK) {
        ACLLITE_LOG_ERROR("[ERROR] acldvppSetBorderConfigTop failed. ERROR: %d", aclRet);
        return atlRet;
    }
    aclRet = acldvppSetBorderConfigRight(borderConfig_, borderRightOffset);
    if (atlRet != ACLLITE_OK) {
        ACLLITE_LOG_ERROR("[ERROR] acldvppSetBorderConfigRight failed. ERROR: %d", aclRet);
        return atlRet;
    }
    aclRet = acldvppSetBorderConfigBottom(borderConfig_, borderBottomOffset);
    if (atlRet != ACLLITE_OK) {
        ACLLITE_LOG_ERROR("[ERROR] acldvppSetBorderConfigBottom failed. ERROR: %d", aclRet);
        return atlRet;
    }
    for(uint32_t borderIndex = 0; borderIndex < 3; borderIndex++)
    {
        aclRet = acldvppSetBorderConfigValue(borderConfig_, borderIndex, borderValue[borderIndex])
        if (atlRet != ACLLITE_OK) {
            ACLLITE_LOG_ERROR("[ERROR] acldvppSetBorderConfigValue failed. ERROR: %d", aclRet);
            return atlRet;
        }
    }

    // border pic
    aclError aclRet = acldvppVpcMakeBorderAsync(dvppChannelDesc_, vpcInputDesc_,
                                            vpcOutputDesc_, borderConfig_, stream_);
    if (aclRet != ACL_SUCCESS) {
        ACLLITE_LOG_ERROR("acldvppVpcBorderAsync failed, error: %d", aclRet);
        return ACLLITE_ERROR_RESIZE_ASYNC;
    }

    aclRet = aclrtSynchronizeStream(stream_);
    if (aclRet != ACL_SUCCESS) {
        ACLLITE_LOG_ERROR("border aclrtSynchronizeStream failed, error: %d", aclRet);
        return ACLLITE_ERROR_SYNC_STREAM;
    }
    borderImage.format = PIXEL_FORMAT_YUV_SEMIPLANAR_420;
    borderImage.width = size_.width;
    borderImage.height = size_.height;
    borderImage.alignWidth = ALIGN_UP16(size_.width);
    borderImage.alignHeight = ALIGN_UP2(size_.height);
    borderImage.size = vpcOutBufferSize_;
    borderImage.data = SHARED_PTR_DVPP_BUF(vpcOutBufferDev_);

    DestroyBorderResource();

    return ACLLITE_OK;
}

void BorderHelper::DestroyBorderResource()
{
    if (borderConfig_ != nullptr) {
        (void)acldvppDestroyBorderConfig(borderConfig_);
        borderConfig_ = nullptr;
    }

    if (vpcInputDesc_ != nullptr) {
        (void)acldvppDestroyPicDesc(vpcInputDesc_);
        vpcInputDesc_ = nullptr;
    }

    if (vpcOutputDesc_ != nullptr) {
        (void)acldvppDestroyPicDesc(vpcOutputDesc_);
        vpcOutputDesc_ = nullptr;
    }
}
