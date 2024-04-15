#include "stdafx.h"
#include "View.h"

using namespace Windows::Foundation;

View::View(UINT_PTR pSample) :
    app_(reinterpret_cast<render_system*>(pSample)),
    window_closed_(false)
{
}

void View::Initialize(CoreApplicationView^ applicationView)
{
    applicationView->Activated += ref new TypedEventHandler<CoreApplicationView^, IActivatedEventArgs^>(this, &View::OnActivated);

    // For simplicity, this sample ignores CoreApplication's Suspend and Resume
    // events which a typical app_ should subscribe to.
}

void View::SetWindow(CoreWindow^ window)
{
    window->KeyDown += ref new TypedEventHandler<CoreWindow^, KeyEventArgs^>(this, &View::OnKeyDown);
    window->KeyUp += ref new TypedEventHandler<CoreWindow^, KeyEventArgs^>(this, &View::OnKeyUp);
    window->Closed += ref new TypedEventHandler<CoreWindow^, CoreWindowEventArgs^>(this, &View::OnClosed);

    // For simplicity, this sample ignores a number of events on CoreWindow that a
    // typical app_ should subscribe to.
}

void View::Load(String^ /*entryPoint*/)
{
}

void View::Run()
{
    auto applicationView = ApplicationView::GetForCurrentView();
    applicationView->Title = ref new Platform::String(app_->title());

    app_->init();

    while (!window_closed_) {
        CoreWindow::GetForCurrentThread()->Dispatcher->ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
        
        app_->update();
        app_->render();
    }

    app_->cleanup();
}

void View::Uninitialize()
{
}

void View::OnActivated(CoreApplicationView^ /*applicationView*/, IActivatedEventArgs^ args)
{
    CoreWindow::GetForCurrentThread()->Activate();
}

void View::OnKeyDown(CoreWindow^ /*window*/, KeyEventArgs^ args)
{
    if (static_cast<UINT>(args->VirtualKey) < 256)
    {
        app_->key_down(static_cast<UINT8>(args->VirtualKey));
    }
}

void View::OnKeyUp(CoreWindow^ /*window*/, KeyEventArgs^ args)
{
    if (static_cast<UINT>(args->VirtualKey) < 256)
    {
        app_->key_up(static_cast<UINT8>(args->VirtualKey));
    }
}

void View::OnClosed(CoreWindow^ /*sender*/, CoreWindowEventArgs^ /*args*/)
{
    window_closed_ = true;
}
