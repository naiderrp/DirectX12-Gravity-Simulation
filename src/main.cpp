#include "stdafx.h"
#include "ViewProvider.h"
#include "render_system.hpp"

int WINAPIV main(Platform::Array<Platform::String^>^ /*params*/) {
    render_system sample(1920, 1080, L""); // 1200, 900
    auto viewProvider = ref new ViewProvider(reinterpret_cast<UINT_PTR>(&sample));

    Windows::ApplicationModel::Core::CoreApplication::Run(viewProvider);
    return 0;
}
