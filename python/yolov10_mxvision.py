from mindx.sdk import base
from mindx.sdk.base import ImageProcessor, Size, Model, Dim, Color

import numpy as np
import cv2

from utils import draw_box


def preprocess(device_id, image_path, model_width, model_height):
    # 图像解码  
    # 初始化ImageProcessor对象  
    imageProcessor = ImageProcessor(device_id)
    # 读取图片路径进行解码，解码格式为nv12（YUV_SP_420）  
    decoded_image = imageProcessor.decode(image_path, base.rgb) 

    # 图像缩放  
    # 计算缩放比例，以保持模型输入尺寸不变
    ratio = min(model_width/decoded_image.width, model_height/decoded_image.height)
    # 计算缩放后需要添加的边框宽度和高度
    border_w = int(round(decoded_image.width * ratio))
    border_h = int(round(decoded_image.height * ratio))

   # 缩放尺寸
    size_para = Size(border_w, border_h)  
    # 读取将解码后的Image类按尺寸进行缩放，缩放方式为华为自研的高阶滤波算法（huaweiu_high_order_filter）
    resized_image = imageProcessor.resize(decoded_image, size_para, base.huaweiu_high_order_filter)

    # 图像补边  
    x_offset = (model_width - border_w ) / 2  
    y_offset = (model_height - border_h ) / 2
    # 补边尺寸 
    dim_para = Dim(int(round(x_offset - 0.1)), int(round(x_offset + 0.1)), int(round(y_offset + 0.1)), int(round(y_offset - 0.1)))
    # 读取解码后的Image类按Dim进行补边，补边方式为重复最后一个元素  
    padded_image = imageProcessor.padding(resized_image, dim_para, Color(114, 114, 114), base.border_constant)


    # Image类转Tensor类
    output_tensor = padded_image.to_tensor()

    return output_tensor, ratio, x_offset, y_offset

def infer(device_id, model_path, input_tensor):
     # 构造输入Tensor类并转移至device侧  
    input_tensor.to_device(device_id)
    # 初始化Model类  
    model = Model(modelPath=model_path, deviceId=device_id)
    # 执行推理  
    output = model.infer(input_tensor)

    return output

def postprocess(output, ratio, dw, dh):
    result = []
    for i in range(output.shape[0]):
        # 读取类别置信度
        confidence = output[i][4]
        # 用阈值进行过滤
        if confidence > 0.5:
            # 读取类别索引
            label = int(output[i][5])
            # 读取类坐标值，把坐标还原到原始图像
            xmin = int((output[i][0] - int(round(dw - 0.1))) / ratio)
            ymin = int((output[i][1] - int(round(dh - 0.1))) / ratio)
            xmax = int((output[i][2] - int(round(dw + 0.1))) / ratio)
            ymax = int((output[i][3] - int(round(dh + 0.1))) / ratio)
            result.append([xmin, ymin, xmax, ymax, confidence, label])

    return result

if __name__ == '__main__':
    image_path = './test.jpg'
    model_path = './models/yolov10m_normal_310P3.om'
    # 执行全局初始化后即可正常调用mxVision接口
    base.mx_init()
    # mxVision预处理
    input_tensor, ratio, dw, dh = preprocess(0, image_path, 640, 640)
    # mxVision推理
    output = infer(0, model_path, input_tensor)[0]
    output.to_host()
    # 执行全局去初始化释放资源
    base.mx_deinit()

    # 后处理
    output = np.array(output)
    output = np.squeeze(np.array(output)[0])
    result = postprocess(output, ratio, dw, dh)

    # 可视化
    img = cv2.imread(image_path)
    for xmin, ymin, xmax, ymax, confidence, label in result:
        draw_box(img, [xmin, ymin, xmax, ymax], confidence, label)
    
    cv2.imwrite('result.jpg', img)
    print('save done')
