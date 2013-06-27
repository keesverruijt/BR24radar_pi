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
//#include <wx/glcanvas.h>
#include "wx/datetime.h"
//#include <wx/fileconf.h>
//#include <fstream>
//#include "chart1.h"
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
    ID_OK_Z,
	ID_ALARMZONES
};

//---------------------------------------------------------------------------------------
//          Alarm Controls Implementation
//---------------------------------------------------------------------------------------
IMPLEMENT_CLASS(AlarmZoneDialog, wxDialog)

BEGIN_EVENT_TABLE(AlarmZoneDialog, wxDialog)

    EVT_CLOSE(AlarmZoneDialog::OnClose)
    EVT_BUTTON(ID_OK_Z, AlarmZoneDialog::OnIdOKClick)
    EVT_MOVE(AlarmZoneDialog::OnMove)
    EVT_SIZE(AlarmZoneDialog::OnSize)

END_EVENT_TABLE()

AlarmZoneDialog::AlarmZoneDialog()
{
    Init();
}

AlarmZoneDialog::~AlarmZoneDialog()
{
}

void AlarmZoneDialog::Init()
{    
    m_AlarmZone_dialog_x = 0;
    m_AlarmZone_dialog_y = 0;
    m_AlarmZone_dialog_sx = 200;
    m_AlarmZone_dialog_sy = 200;
}

bool AlarmZoneDialog::Create
(
    wxWindow        *parent,
    br24radar_pi    *pPI,
    wxWindowID      id,
    const wxString  &m_caption,
    const wxPoint   &pos,
    const wxSize    &size,
    long            style
)
{
    pParent = parent;
    pPlugIn = pPI;

    long    wstyle = wxDEFAULT_FRAME_STYLE;

    wxSize  size_min = size;

    if(!wxDialog::Create(parent, id, m_caption, pos, size_min, wstyle)) return false;

    CreateControls();

    DimeWindow(this);

    Fit();
    SetMinSize(GetBestSize());

    return true;
}

void AlarmZoneDialog::CreateControls()
{
    int border_size = 4;

    wxBoxSizer  *AlarmZoneSizer = new wxBoxSizer(wxVERTICAL);
    SetSizer(AlarmZoneSizer);

    // Alarm Zone options 
    wxStaticBox         *BoxAlarmZone = new wxStaticBox(this, wxID_ANY, _("Alarm Zones"));
    wxStaticBoxSizer    *BoxAlarmZoneSizer = new wxStaticBoxSizer(BoxAlarmZone, wxVERTICAL);

    AlarmZoneSizer->Add(BoxAlarmZoneSizer, 0, wxEXPAND | wxALL, border_size);

    wxString    ZoneTypeStrings[] =
    {
        _("Arc"),
        _("Circle")
    };

    pAlarmZoneSelect = new wxRadioBox (this, ID_ALARMZONES, _("Alarm Zone:"),
                                            wxDefaultPosition, wxDefaultSize,
                                            2, ZoneTypeStrings, 1, wxRA_SPECIFY_COLS );

    BoxAlarmZoneSizer->Add(pAlarmZoneSelect, 0, wxALL | wxEXPAND, 2);

    pAlarmZoneSelect->Connect( wxEVT_COMMAND_RADIOBOX_SELECTED,
                                wxCommandEventHandler(AlarmZoneDialog::OnAlarmZoneModeClick),
                                NULL, this );

    pAlarmZoneSelect->SetSelection(0);

    
    // The Close button 
    wxButton    *bClose = new wxButton(this, ID_OK_Z, _("&Close"), wxDefaultPosition, wxDefaultSize, 0);
    AlarmZoneSizer->Add(bClose, 0, wxALIGN_CENTER_VERTICAL | wxALL, 5);
}

void AlarmZoneDialog::OnAlarmZoneModeClick(wxCommandEvent &event)
{
 //   br_AlarmZone_index = pAlarmZoneSelect->GetSelection();
//    pPlugIn->SetAlarmZoneMode(br_AlarmZone_index);

//    wxString        message(_("AlarmZone: %d"), br_AlarmZone_index);
//    wxMessageDialog dlg(GetOCPNCanvasWindow(), message, _T("BR24 Radar"));

}

void AlarmZoneDialog::OnClose(wxCloseEvent &event)
{
    pPlugIn->OnAlarmZoneDialogClose();
    event.Skip();
}

void AlarmZoneDialog::OnIdOKClick(wxCommandEvent &event)
{
    pPlugIn->OnAlarmZoneDialogClose();
    event.Skip();
}

void AlarmZoneDialog::OnMove(wxMoveEvent &event)
{

    // Record the dialog position
    wxPoint p = GetPosition();

//    pPlugIn->SetAlarmZoneDialogX(p.x);
//    pPlugIn->SetAlarmZoneDialogY(p.y);

    event.Skip();
}

void AlarmZoneDialog::OnSize(wxSizeEvent &event)
{

    // Record the dialog size 
    wxSize  p = event.GetSize();

 //   pPlugIn->SetAlarmZoneDialogSizeX(p.x);
//    pPlugIn->SetAlarmZoneDialogSizeY(p.y);

    event.Skip();
}
