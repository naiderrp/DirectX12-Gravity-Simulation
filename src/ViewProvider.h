#pragma once

using namespace Windows::ApplicationModel::Core;

ref class ViewProvider sealed : IFrameworkViewSource
{
public:
    ViewProvider(UINT_PTR pSample);
    virtual IFrameworkView^ CreateView();

private:
    UINT_PTR app_;
};
