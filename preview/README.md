# RLCD-4.2 Dashboard 预览(Mac)

在 Mac 上预览 dashboard 布局,无需烧录。

## 前置依赖

- macOS + SDL2:`brew install sdl2`
- CMake + make(系统自带)

## 构建

```bash
cd preview
mkdir build && cd build
cmake ..
make
```

## 运行

```bash
./dashboard_preview
```

## 改布局

编辑 `../main/boards/waveshare/esp32-s3-rlcd-4.2/dashboard_layout.cc`(固件和预览共享),
然后在 build 目录里 `make` 重新编译(秒级),重新运行即可看效果。
