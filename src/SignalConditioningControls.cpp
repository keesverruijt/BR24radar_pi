/******************************************************************************
 *
 * Project:  OpenCPN
 * Purpose:  Navico BR24 Radar Plugin
 * Author:   David Register
 *           Dave Cowell
 *           Kees Verruijt
 *
 ***************************************************************************
 *   Copyright (C) 2010 by David S. Register              bdbcat@yahoo.com *
 *   Copyright (C) 2012-2013 by Dave Cowell                                *
 *   Copyright (C) 2012-2013 by Kees Verruijt         canboat@verruijt.net *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************
 */

#include "wx/wxprec.h"

#ifndef  WX_PRECOMP
#include "wx/wx.h"
#endif                          //precompiled headers

#include <wx/socket.h>
#include "wx/apptrait.h"
#include "wx/datetime.h"
using namespace std;

#ifdef __WXGTK__
#include <netinet/in.h>
#include <sys/ioctl.h>
#endif

#ifdef __WXMSW__
#include "GL/glu.h"
#endif

#include "br24radar_pi.h"


enum {                                      // process ID's
    ID_OK,
    ID_CLUTTER,
    ID_GAIN,
    ID_REJECTION
};

//---------------------------------------------------------------------------------------
//          Radar Signal Conditioning Implementation
//---------------------------------------------------------------------------------------
IMPLEMENT_CLASS(SignalConditioningDialog, wxDialog)

BEGIN_EVENT_TABLE(SignalConditioningDialog, wxDialog)

    EVT_CLOSE(SignalConditioningDialog::OnClose)
    EVT_BUTTON(ID_OK,SignalConditioningDialog::OnIdOKClick)
    EVT_RADIOBUTTON(ID_CLUTTER, SignalConditioningDialog::OnFilterProcessClick)
    EVT_RADIOBUTTON(ID_REJECTION, SignalConditioningDialog::OnRejectionModeClick)


END_EVENT_TABLE()



   SignalConditioningDialog::SignalConditioningDialog()
{
    Init();
}


   SignalConditioningDialog::~SignalConditioningDialog()
{
}


void SignalConditioningDialog::Init()
{
}

bool SignalConditioningDialog::Create(wxWindow *parent, br24radar_pi *ppi, wxWindowID id,
                                const wxString &m_caption,
                                const wxPoint& pos, const wxSize& size, long style)
{

    pParent = parent;
    pPlugIn = ppi;

    long    wstyle = wxDEFAULT_FRAME_STYLE;

    wxSize  size_min = size;

    if(!wxDialog::Create(parent, id, m_caption, pos, size_min, wstyle)) return false;

    CreateControls();

    DimeWindow(this);

    Fit();
    SetMinSize(GetBestSize());

    return true;
}

void SignalConditioningDialog::CreateControls()
{
    int border_size = 4;

    wxBoxSizer  *SignalConditioningSizer = new wxBoxSizer(wxVERTICAL);
    SetSizer(SignalConditioningSizer);

//  Image Conditioning Options
    wxStaticBox* BoxSignalConditioning = new wxStaticBox(this, wxID_ANY, _("Signal Conditioning"));
    wxStaticBoxSizer* BoxSignalConditioningSizer = new wxStaticBoxSizer(BoxSignalConditioning, wxVERTICAL);
    SignalConditioningSizer->Add(BoxSignalConditioningSizer, 0, wxEXPAND | wxALL, border_size);

// Rejection settings
    wxString RejectionStrings[] = {
        _("Off"),
        _("Low"),
        _("Medium"),
        _("High"),
    };

    pRejectionMode = new wxRadioBox(this, ID_REJECTION, _("Rejection"),
                                    wxDefaultPosition, wxDefaultSize,
                                    sizeof(RejectionStrings)/sizeof(RejectionStrings[0]), RejectionStrings, 1, wxRA_SPECIFY_COLS);

    BoxSignalConditioningSizer->Add(pRejectionMode, 0, wxALL | wxEXPAND, 2);

    pRejectionMode->Connect(wxEVT_COMMAND_RADIOBOX_SELECTED,
                            wxCommandEventHandler(SignalConditioningDialog::OnRejectionModeClick), NULL, this);

    pRejectionMode->SetSelection(pPlugIn->settings.rejection);

//  Clutter Options
    wxString FilterProcessStrings[] = {
        _("Auto Gain"),
        _("Manual Gain"),
        _("Rain Clutter - Manual"),
        _("Sea Clutter - Auto"),
        _("Sea Clutter - Manual"),
    };

    pFilterProcess = new wxRadioBox(this, ID_CLUTTER, _("Tuning"),
                                    wxDefaultPosition, wxDefaultSize,
                                    5, FilterProcessStrings, 1, wxRA_SPECIFY_COLS);

    BoxSignalConditioningSizer->Add(pFilterProcess, 0, wxALL | wxEXPAND, 2);
    pFilterProcess->Connect(wxEVT_COMMAND_RADIOBOX_SELECTED,
                            wxCommandEventHandler(SignalConditioningDialog::OnFilterProcessClick), NULL, this);

//  Gain slider

    wxStaticBox* BoxGain = new wxStaticBox(this, wxID_ANY, _("Gain"));
    wxStaticBoxSizer* sliderGainsizer = new wxStaticBoxSizer(BoxGain, wxVERTICAL);
    BoxSignalConditioningSizer->Add(sliderGainsizer, 0, wxALL | wxEXPAND, 2);

    pGainSlider = new wxSlider(this, ID_GAIN, 50, 1, 100, wxDefaultPosition,  wxDefaultSize,
                               wxSL_HORIZONTAL|wxSL_LABELS,  wxDefaultValidator, _("slider"));

    sliderGainsizer->Add(pGainSlider, 0, wxALL | wxEXPAND, 2);

    pGainSlider->Connect(wxEVT_SCROLL_CHANGED,
                         wxCommandEventHandler(SignalConditioningDialog::OnGainSlider), NULL, this);



// A horizontal box sizer to contain OK
    wxBoxSizer* AckBox = new wxBoxSizer(wxHORIZONTAL);
    BoxSignalConditioningSizer->Add(AckBox, 0, wxALIGN_CENTER_HORIZONTAL | wxALL, 5);

// The OK button
    wxButton* bOK = new wxButton(this, ID_OK, _("&Close"),
                                 wxDefaultPosition, wxDefaultSize, 0);
    AckBox->Add(bOK, 0, wxALIGN_CENTER_VERTICAL | wxALL, 5);
}


void SignalConditioningDialog::OnFilterProcessClick(wxCommandEvent &event)
{
    int sel_gain = 0;

    pPlugIn->settings.filter_process = pFilterProcess->GetSelection();
    switch (pPlugIn->settings.filter_process) {
        case 0: {                                       //Gain Auto
                pPlugIn->m_pControlDialog->SetGainText(false);
                pGainSlider->Disable();
                break;
            }
        case 1: {                                       //Manual Gain
                sel_gain = pPlugIn->settings.gain;
                pGainSlider->Enable();
                break;
            }
        case 2: {                                       //Rain Clutter Man
                sel_gain = pPlugIn->settings.rain_clutter_gain;
                pGainSlider->Enable();
                break;
            }
        case 3: {                                       // Sea Clutter Auto
                pPlugIn->m_pControlDialog->SetSeaClutterText(false);
                pGainSlider->Disable();
                break;
            }        
        case 4: {                                       //Sea Clutter Man
                sel_gain = pPlugIn->settings.sea_clutter_gain;
                pGainSlider->Enable();
                break;
            }
    }
    pGainSlider->SetValue(sel_gain);

//    pPlugIn->SetFilterProcess(pPlugIn->settings.filter_process, sel_gain);
}

void SignalConditioningDialog::OnRejectionModeClick(wxCommandEvent &event)
{
    int rejection_mode = pRejectionMode->GetSelection();
    pPlugIn->settings.rejection = rejection_mode;
    pPlugIn->SetRejectionMode(rejection_mode);
}

void SignalConditioningDialog::OnGainSlider(wxCommandEvent &event)
{
    int sel_gain = pGainSlider->GetValue();


    switch (pPlugIn->settings.filter_process) {
        case 1: {                                   //Gain Man
                pPlugIn->settings.gain = sel_gain;
                pPlugIn->m_pControlDialog->SetGainText(true);
                break;
                }
        case 2: {                                   //Rain Cutter Man
                pPlugIn->settings.rain_clutter_gain = sel_gain;
                pPlugIn->m_pControlDialog->SetRainClutterText();
                break;
                }
        case 4: {                                   //Sea Clutter Man
                //sel_gain = sel_gain * 0x50 / 0x100;
                pPlugIn->settings.sea_clutter_gain = sel_gain;
                pPlugIn->m_pControlDialog->SetSeaClutterText(true);
                break;
            }
    } 
    pPlugIn->SetFilterProcess(pPlugIn->settings.filter_process, sel_gain);
}

void SignalConditioningDialog::OnClose(wxCloseEvent& event)
{
    pPlugIn->OnSignalConditioningDialogClose();
    event.Skip();
}


void SignalConditioningDialog::OnIdOKClick(wxCommandEvent& event)
{
    pPlugIn->OnSignalConditioningDialogClose();
    event.Skip();
}