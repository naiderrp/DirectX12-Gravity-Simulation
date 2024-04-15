#pragma once

#include "render_system.hpp"

using namespace Platform;
using namespace Windows::ApplicationModel;
using namespace Windows::ApplicationModel::Activation;
using namespace Windows::ApplicationModel::Core;
using namespace Windows::UI::Core;
using namespace Windows::UI::ViewManagement;

ref class View sealed : public IFrameworkView
{
public:
    View(UINT_PTR pSample);

    virtual void Initialize(CoreApplicationView^ applicationView);
    virtual void SetWindow(CoreWindow^ window);
    virtual void Load(String^ entryPoint);
    virtual void Run();
    virtual void Uninitialize();

private:
    void OnActivated(CoreApplicationView^ applicationView, IActivatedEventArgs^ args);
    void OnKeyDown(CoreWindow^ window, KeyEventArgs^ keyEventArgs);
    void OnKeyUp(CoreWindow^ window, KeyEventArgs^ keyEventArgs);
    void OnClosed(CoreWindow ^sender, CoreWindowEventArgs ^args);

    render_system* app_;
    bool window_closed_;
};
