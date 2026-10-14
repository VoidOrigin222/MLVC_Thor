# C++ 格式与命名

第一方 C++/CUDA 文件使用 Google clang-format 风格：两空格缩进、80 列、
左侧指针星号、按类别整理头文件。当前检查使用 clang-format 20.1.8。
第三方源码不参加格式化。

```bash
bash scripts/format_cpp.sh
bash scripts/format_cpp.sh --check
```

头文件使用项目路径前缀的 include guard。类的私有成员使用尾部下划线；
结构体字段使用 snake_case；类型、函数使用 PascalCase。调用 NVIDIA、
FFmpeg、CUDA、ACL 兼容层时保留 SDK 定义的名称、回调签名和 ABI。
直接包含使用的标准库头文件，不依赖其他头文件的间接包含。

本次按要求保留异常机制，继续使用 C++17。因此并不声称完全符合 Google
规范的所有规则。Thor 新增代码通过以下 cpplint 检查；关闭的类别分别对应
未补写版权声明、项目 guard 命名、C++17、SDK include 路径和现有引用接口。

```bash
python3 -m cpplint --recursive --extensions=cc,h,cu,cuh \
  --filter=-legal/copyright,-build/header_guard,-build/c++11,-build/c++17,-build/include_subdir,-runtime/references \
  ports/thor
```

格式、命名调整不改变算法参数。Thor 视频输入只允许 NVDEC，视频编码只使用
NVENC，已去掉 libx264 编码及 OpenCV 视频解码回退。用于运动向量提取的
H.264 CPU 解码保留；CPU 熵编解码也保留。相机继续使用原 V4L2 接口。
旧 Ascend 实现保留在独立的 ACL 构建目标中。
