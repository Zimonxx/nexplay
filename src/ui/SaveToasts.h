#pragma once
#include "SaveToastModel.h"
#include <Windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <memory>

namespace nexplay::ui {
// Same painter is used by native overlays and offscreen visual regression tests.
void drawSaveToast(ID2D1RenderTarget *target, IDWriteFactory *textFactory, const ToastEntry &entry,
                   D2D1_COLOR_F accent);
class SaveToasts {
  public:
    SaveToasts();
    ~SaveToasts();
    SaveToasts(const SaveToasts &) = delete;
    SaveToasts &operator=(const SaveToasts &) = delete;
    void configure(ToastCorner corner, D2D1_COLOR_F accent);
    void update(app::SaveProgress progress);
    void close();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace nexplay::ui
