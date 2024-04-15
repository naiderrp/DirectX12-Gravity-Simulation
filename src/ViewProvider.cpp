#include "stdafx.h"

#include "ViewProvider.h"
#include "View.h"

ViewProvider::ViewProvider(UINT_PTR pSample) :
    app_(pSample)
{
}

Windows::ApplicationModel::Core::IFrameworkView^ ViewProvider::CreateView()
{
    return ref new View(app_);
}
