/******************************************************************************
 *
 * Project:  OpenCPN
 * Purpose:  Navico BR24 Radar Plugin
 * Author:   David Register
 *           Dave Cowell
 *           Kees Verruijt
 *           Douwe Fokkema
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

/*

 Additional contributors for release 1.2, spring 2015

 Douwe Fokkema for
 - Heading on radar. Use the heading that the radar puts in the line headers when it is getting a heading from the RI10 or
 RI11 interface box
 - When there is no heading on radar, the heading at the time of receiving the data will be used for the radar image.
 - Performance improvements of the display process, allowing for rapid refresh
 - Rapid refresh of the radar image. You can now see the beam rotating.
 - Changes of the range settings. Displayed ranges will now correspond better with the ranges that a HDS display shows.
 - Smoother switching on of the 4G radar
 - Various smaller corrections

 H�kan Svensson for
 - Timed transmit function
 - Many tests
 - Translation files

 */


#ifdef _WINDOWS
# include <WinSock2.h>
# include <ws2tcpip.h>
# pragma comment (lib, "Ws2_32.lib")
#endif

#include "wx/wxprec.h"

#ifndef  WX_PRECOMP
#include "wx/wx.h"
#endif                          //precompiled headers

#include <wx/socket.h>
#include "wx/apptrait.h"
#include <wx/glcanvas.h>
#include "wx/sckaddr.h"
#include "wx/datetime.h"
#include <wx/fileconf.h>
#include <fstream>

using namespace std;

#ifdef __WXGTK__
# include <netinet/in.h>
# include <sys/ioctl.h>
#endif

#ifdef __WXOSX__
# include <sys/types.h>
# include <sys/socket.h>
# include <netdb.h>
#endif

#ifdef __WXMSW__
# include "GL/glu.h"
#endif

#include "br24radar_pi.h"
//#include "ocpndc.h"


// A marker that uniquely identifies BR24 generation scanners, as opposed to 4G(eneration)
// Note that 3G scanners are BR24's with better power, so they are more BR23+ than 4G-.
// As far as we know they 3G's use exactly the same command set.

// If BR24MARK is found, we switch to BR24 mode, otherwise 4G.
static UINT8 BR24MARK[] = { 0x00, 0x44, 0x0d, 0x0e };

int bogey_count[4];
static int displaysetting_threshold[3] = {displaysetting0_threshold_red, displaysetting1_threshold_blue, displaysetting2_threshold_blue};

enum {
    // process ID's
    ID_OK,
    ID_RANGE_UNITS,
    ID_OVERLAYDISPLAYOPTION,
    ID_DISPLAYTYPE,
    ID_HEADINGSLIDER,
    ID_SELECT_SOUND,
    ID_TEST_SOUND,
    ID_PASS_HEADING,
    ID_SELECT_AB,
    ID_EMULATOR
};
enum {RED, AMBER, GREEN };

static int toolbar_button = RED;

bool br_bpos_set = false;
double br_ownship_lat, br_ownship_lon;
double br_cur_lat, br_cur_lon;
double br_hdm;
double br_hdt;     // this is the heading that the pi is using for all heading operations, in degrees
                   // br_hdt will come from the radar if available else from the NMEA stream

int br_hdt_raw = 0;// if set by radar, the heading (in 0..4095)

// Variation. Used to convert magnetic into true heading.
// Can come from SetPositionFixEx, which may hail from the WMM plugin
// and is thus to be preferred, or GPS or a NMEA sentence. The latter will probably
// have an outdated variation model, so is less preferred. Besides, some devices
// transmit invalid (zero) values. So we also let non-zero values prevail.
double br_var = 0.0;     // local magnetic variation, in degrees
enum VariationSource { VARIATION_SOURCE_NONE, VARIATION_SOURCE_NMEA, VARIATION_SOURCE_FIX, VARIATION_SOURCE_WMM };
VariationSource br_var_source = VARIATION_SOURCE_NONE;


bool br_heading_on_radar = false;
unsigned int br_refresh_rate = 1;  // refreshrate for radar used in process buffer
static const unsigned int REFRESHMAPPING[] = { 10, 9, 3, 1, 0}; // translation table for the refreshrate, interval between received frames
// user values 1 to 5 mapped to these values for refrehs interval
// user 1 - no additional refresh, 2 - interval between frames 9, so on.
volatile bool br_refresh_busy_or_queued = false;

double br_mark_rng = 0, br_mark_brg = 0;      // This is needed for context operation
int br_range_meters[2] = { 0, 0 };           // current range for radar
int br_commanded_range_meters = 0; // Range that the plugin to the radar
int br_auto_range_meters = 0;      // What the range should be, at least, when AUTO mode is selected
int br_previous_auto_range_meters = 0;
bool br_update_range_control[2] = { false, false };
bool br_update_address_control = false;
bool br_update_error_control = false;
wxString br_ip_address; // Current IP address of the ethernet interface that we're doing multicast receive on.
wxString br_error_msg;

//Timed Transmit
bool br_init_timed_transmit;
static time_t br_idle_watchdog;
int br_idle_dialog_time_left = 999;
int TimedTransmit_IdleBoxMode;
int time_left = 0;                     

int   br_scanner_state = RADAR_OFF;
RadarType br_radar_type = RT_4G;  // default value

static bool  br_radar_seen = false;
static int my_address;
static bool  previous_br_radar_seen = false;
static double gLon, gLat;   // used for the initial boat position as read from ini file
static bool  br_data_seen = false;
static bool  br_opengl_mode = false;
static time_t      br_bpos_watchdog;
static time_t      br_hdt_watchdog;
static time_t      br_radar_watchdog;
static time_t      br_data_watchdog;
static time_t      br_var_watchdog;
static bool blackout[2] = { false, false };         //  will force display to blackout and north up

// for VBO operation
static GLuint vboId = 0;

#define     SIZE_VERTICES (3072)
static GLfloat vertices[2048][SIZE_VERTICES];
static int colors_index[2048];
static time_t vertices_time_stamp[2048];
static int vertices_index[2048];

#define     WATCHDOG_TIMEOUT (10)  // After 10s assume GPS and heading data is invalid
#define     TIMER_NOT_ELAPSED(watchdog) (now < watchdog + WATCHDOG_TIMEOUT)
#define     TIMER_ELAPSED(watchdog) (!TIMER_NOT_ELAPSED(watchdog))
time_t      br_dt_stayalive;
#define     STAYALIVE_TIMEOUT (5)  // Send data every 5 seconds to ping radar

int   br_radar_control_id = 0, br_guard_zone_id = 0;
bool  br_guard_context_mode;

static GLfloat polar_to_cart_x[2049][513];
static GLfloat polar_to_cart_y[2049][513];

bool        br_guard_bogey_confirmed = false;
time_t      br_alarm_sound_last;
#define     ALARM_TIMEOUT (10)

static sockaddr_in * br_mcast_addr = 0; // One of the threads finds out where the radar lives and writes our IP here
static sockaddr_in * br_radar_addr = 0; // One of the threads finds out where the radar lives and writes its IP here
// static wxCriticalSection br_scanLock;

// the class factories, used to create and destroy instances of the PlugIn

extern "C" DECL_EXP opencpn_plugin* create_pi(void *ppimgr)
{
    return new br24radar_pi(ppimgr);
}

extern "C" DECL_EXP void destroy_pi(opencpn_plugin* p)
{
    delete p;
}


/********************************************************************************************************/
//   Distance measurement for simple sphere
/********************************************************************************************************/
//static double twoPI = 2 * PI;

static double deg2rad(double deg)
{
    return (deg * PI / 180.0);
}

static double rad2deg(double rad)
{
    return (rad * 180.0 / PI);
}

static double local_distance (double lat1, double lon1, double lat2, double lon2) {
    // Spherical Law of Cosines
    double theta, dist;

    theta = lon2 - lon1;
    dist = sin(deg2rad(lat1)) * sin(deg2rad(lat2)) + cos(deg2rad(lat1)) * cos(deg2rad(lat2)) * cos(deg2rad(theta));
    dist = acos(dist);        // radians
    dist = rad2deg(dist);
    dist = fabs(dist) * 60    ;    // nautical miles/degree
    return (dist);
}

static double radar_distance(double lat1, double lon1, double lat2, double lon2, char unit)
{
    double dist = local_distance (lat1, lon1, lat2, lon2);

    switch (unit) {
        case 'M':              // statute miles
            dist = dist * 1.1515;
            break;
        case 'K':              // kilometers
            dist = dist * 1.852;
            break;
        case 'm':              // meters
            dist = dist * 1852.0;
            break;
        case 'N':              // nautical miles
            break;
    }
    //    wxLogMessage(wxT("Radar Distance %f,%f,%f,%f,%f, %c"), lat1, lon1, lat2, lon2, dist, unit);
    return dist;
}

static double local_bearing (double lat1, double lon1, double lat2, double lon2) //FES
{
    double angle = atan2(deg2rad(lat2-lat1), (deg2rad(lon2-lon1) * cos(deg2rad(lat1))));

    angle = rad2deg(angle);
    angle = MOD_DEGREES(90.0 - rad2deg(angle));
    return angle;
}


static void draw_blob_gl_i(int arc, int radius, int radius_end, GLubyte red, GLubyte green, GLubyte blue, GLubyte alpha)
{
    int arc_end = arc + 1;
    if (arc_end >= 2048){
        arc_end = arc_end - 2048;
    }
    vertices[arc][vertices_index[arc]] = polar_to_cart_x[arc][radius];   // A
    vertices_index[arc]++;
    vertices[arc][vertices_index[arc]] = polar_to_cart_y[arc][radius];
    vertices_index[arc]++;

    vertices[arc][vertices_index[arc]] = (GLfloat)red;    // colors of A
    vertices_index[arc]++;
    vertices[arc][vertices_index[arc]] = (GLfloat)green;
    vertices_index[arc]++;
    vertices[arc][vertices_index[arc]] = (GLfloat)blue;
    vertices_index[arc]++;
    vertices[arc][vertices_index[arc]] = (GLfloat)((GLfloat)alpha) / 255.;
    vertices_index[arc]++;

    vertices[arc][vertices_index[arc]] = polar_to_cart_x[arc][radius_end];  // B
    vertices_index[arc]++;
    vertices[arc][vertices_index[arc]] = polar_to_cart_y[arc][radius_end];
    vertices_index[arc]++;

    vertices[arc][vertices_index[arc]] = (GLfloat)red;    // colors of B
    vertices_index[arc]++;
    vertices[arc][vertices_index[arc]] = (GLfloat)green;
    vertices_index[arc]++;
    vertices[arc][vertices_index[arc]] = (GLfloat)blue;
    vertices_index[arc]++;
    vertices[arc][vertices_index[arc]] = (GLfloat)((GLfloat)alpha) / 255.;
    vertices_index[arc]++;

    vertices[arc][vertices_index[arc]] = polar_to_cart_x[arc_end][radius];  //  C
    vertices_index[arc]++;
    vertices[arc][vertices_index[arc]] = polar_to_cart_y[arc_end][radius];
    vertices_index[arc]++;

    vertices[arc][vertices_index[arc]] = (GLfloat)red;    // colors of C
    vertices_index[arc]++;
    vertices[arc][vertices_index[arc]] = (GLfloat)green;
    vertices_index[arc]++;
    vertices[arc][vertices_index[arc]] = (GLfloat)blue;
    vertices_index[arc]++;
    vertices[arc][vertices_index[arc]] = (GLfloat)((GLfloat)alpha) / 255.;
    vertices_index[arc]++;

    //  next triangle follows ----------------------------------------------------------------

    vertices[arc][vertices_index[arc]] = polar_to_cart_x[arc][radius_end];  //B
    vertices_index[arc]++;
    vertices[arc][vertices_index[arc]] = polar_to_cart_y[arc][radius_end];
    vertices_index[arc]++;

    vertices[arc][vertices_index[arc]] = (GLfloat)red;    // colors of B
    vertices_index[arc]++;
    vertices[arc][vertices_index[arc]] = (GLfloat)green;
    vertices_index[arc]++;
    vertices[arc][vertices_index[arc]] = (GLfloat)blue;
    vertices_index[arc]++;
    vertices[arc][vertices_index[arc]] = (GLfloat)((GLfloat)alpha) / 255.;
    vertices_index[arc]++;

    vertices[arc][vertices_index[arc]] = polar_to_cart_x[arc_end][radius];  //  C
    vertices_index[arc]++;
    vertices[arc][vertices_index[arc]] = polar_to_cart_y[arc_end][radius];
    vertices_index[arc]++;

    vertices[arc][vertices_index[arc]] = (GLfloat)red;    // colors of C
    vertices_index[arc]++;
    vertices[arc][vertices_index[arc]] = (GLfloat)green;
    vertices_index[arc]++;
    vertices[arc][vertices_index[arc]] = (GLfloat)blue;
    vertices_index[arc]++;
    vertices[arc][vertices_index[arc]] = (GLfloat)((GLfloat)alpha) / 255.;
    vertices_index[arc]++;

    vertices[arc][vertices_index[arc]] = polar_to_cart_x[arc_end][radius_end];  // D
    vertices_index[arc]++;
    vertices[arc][vertices_index[arc]] = polar_to_cart_y[arc_end][radius_end];
    vertices_index[arc]++;

    vertices[arc][vertices_index[arc]] = (GLfloat)red;    // colors of D
    vertices_index[arc]++;
    vertices[arc][vertices_index[arc]] = (GLfloat)green;
    vertices_index[arc]++;
    vertices[arc][vertices_index[arc]] = (GLfloat)blue;
    vertices_index[arc]++;
    vertices[arc][vertices_index[arc]] = (GLfloat)((GLfloat)alpha) / 255.;
    vertices_index[arc]++;

    if (vertices_index[arc]> SIZE_VERTICES - 36){
        vertices_index[arc] = SIZE_VERTICES - 36;
        wxLogMessage(wxT("BR24radar_pi: vertices array limit overflow vertices_index=%d arc=%d"), vertices_index[arc], arc);
    }
}

static void draw_blob_gl(double ca, double sa, double radius, double arc_width, double blob_heigth)
{
    const double blob_start = 0.0;
    const double blob_end = blob_heigth;

    double xm1 = (radius + blob_start) * ca;
    double ym1 = (radius + blob_start) * sa;
    double xm2 = (radius + blob_end) * ca;
    double ym2 = (radius + blob_end) * sa;

    double arc_width_start2 = (radius + blob_start) * arc_width;
    double arc_width_end2 = (radius + blob_end) * arc_width;

    double xa = xm1 + arc_width_start2 * sa;
    double ya = ym1 - arc_width_start2 * ca;

    double xb = xm2 + arc_width_end2 * sa;
    double yb = ym2 - arc_width_end2 * ca;

    double xc = xm1 - arc_width_start2 * sa;
    double yc = ym1 + arc_width_start2 * ca;

    double xd = xm2 - arc_width_end2 * sa;
    double yd = ym2 + arc_width_end2 * ca;

    glBegin(GL_TRIANGLES);
    glVertex2d(xa, ya);
    glVertex2d(xb, yb);
    glVertex2d(xc, yc);

    glVertex2d(xb, yb);
    glVertex2d(xc, yc);
    glVertex2d(xd, yd);
    glEnd();
}


static void DrawArc(float cx, float cy, float r, float start_angle, float arc_angle, int num_segments)
{
    float theta = arc_angle / float(num_segments - 1); // - 1 comes from the fact that the arc is open

    float tangential_factor = tanf(theta);
    float radial_factor = cosf(theta);

    float x = r * cosf(start_angle);
    float y = r * sinf(start_angle);

    glBegin(GL_LINE_STRIP);
    for(int ii = 0; ii < num_segments; ii++) {
        glVertex2f(x + cx, y + cy);

        float tx = -y;
        float ty = x;

        x += tx * tangential_factor;
        y += ty * tangential_factor;

        x *= radial_factor;
        y *= radial_factor;
    }
    glEnd();
}

static void DrawOutlineArc(double r1, double r2, double a1, double a2, bool stippled)
{
    if (a1 > a2) {
        a2 += 360.0;
    }
    int  segments = (a2 - a1) * 4;
    bool circle = (a1 == 0.0 && a2 == 359.0);

    if (!circle) {
        a1 -= 0.5;
        a2 += 0.5;
    }
    a1 = deg2rad(a1);
    a2 = deg2rad(a2);

    if (stippled) {
        glEnable (GL_LINE_STIPPLE);
        glLineStipple (1, 0x0F0F);
        glLineWidth(2.0);
    } else {
        glLineWidth(3.0);
    }

    DrawArc(0.0, 0.0, r1, a1, a2 - a1, segments);
    DrawArc(0.0, 0.0, r2, a1, a2 - a1, segments);

    if (!circle) {
        glBegin(GL_LINES);
        glVertex2f(r1 * cosf(a1), r1 * sinf(a1));
        glVertex2f(r2 * cosf(a1), r2 * sinf(a1));
        glVertex2f(r1 * cosf(a2), r1 * sinf(a2));
        glVertex2f(r2 * cosf(a2), r2 * sinf(a2));
        glEnd();
    }
}

static void DrawFilledArc(double r1, double r2, double a1, double a2)
{
    if (a1 > a2) {
        a2 += 360.0;
    }

    for (double n = a1; n <= a2; ++n ) {
        double nr = deg2rad(n);
        draw_blob_gl(cos(nr), sin(nr), r2, deg2rad(0.5), r1 - r2);
    }
}

//---------------------------------------------------------------------------------------------------------
//
//    BR24Radar PlugIn Implementation
//
//---------------------------------------------------------------------------------------------------------

#include "icons.h"
//#include "default_pi.xpm"
#include "icons.cpp"

//---------------------------------------------------------------------------------------------------------
//
//          PlugIn initialization and de-init
//
//---------------------------------------------------------------------------------------------------------

br24radar_pi::br24radar_pi(void *ppimgr)
: opencpn_plugin_110(ppimgr)
{
    // Create the PlugIn icons
    initialize_images();
    m_pdeficon = new wxBitmap(*_img_radar_blank);

}

int br24radar_pi::Init(void)
{
    int r;
#ifdef __WXMSW__
    WSADATA wsaData;

    // Initialize Winsock
    r = WSAStartup(MAKEWORD(2,2), &wsaData);
    if (r != 0) {
        wxLogError(wxT("BR24radar_pi: Unable to initialise Windows Sockets, error %d"), r);
        // Might as well give up now
        return 0;
    }
#endif

    
    // initialise polar_to_cart_y[arc + 1][radius] arrays

    for (int arc = 0; arc < 2049; arc++){
        double sine = sin(arc * PI / 1024);
        double cosine = cos(arc * PI / 1024);
        for (int radius = 0; radius < 513; radius++){
            polar_to_cart_y[arc][radius] = (GLfloat) radius * sine;
            polar_to_cart_x[arc][radius] = (GLfloat) radius * cosine;
        }
    }
    double www = -1.;
    wxLogMessage(wxT("BR24radar_pi:Position initialized  xa = %f"), polar_to_cart_x[100][150]);
    AddLocaleCatalog( _T("opencpn-br24radar_pi") );

    m_pControlDialog = NULL;
    m_pMessageBox = NULL;
    settings.selectRadarB = 0;   // temp setting until loadded from ini file
    
    br_scanner_state = RADAR_OFF;                 // Radar scanner is off

    br_dt_stayalive = time(0);
    br_alarm_sound_last = br_dt_stayalive;
    br_bpos_watchdog = 0;
    br_hdt_watchdog  = 0;
    br_var_watchdog = 0;
    br_radar_watchdog = 0;
    br_data_watchdog = 0;
    br_idle_watchdog = 0;
    settings.emulator_on = false;
    memset(bogey_count, 0, sizeof(bogey_count));   // set bogey count 0 
    memset(&radar_setting[0], 0, sizeof(radar_setting));   // radar settings all to 0
    // memset(&settings, 0, sizeof(settings));             // pi settings all 0   // will crash under VC 2010!! OK with 2013

    for (int i = 0; i < LINES_PER_ROTATION - 1; i++) {   // initialise history bytes
        memset(&m_scan_line[0][i].history, 0, sizeof(m_scan_line[0][i].history));
        memset(&m_scan_line[1][i].history, 0, sizeof(m_scan_line[1][i].history));
        }

   if (settings.verbose)wxLogMessage(wxT("BR24radar_pi: size of scanline %d"), sizeof(m_scan_line[0][1].history));
     memset(&m_scan_line[0][LINES_PER_ROTATION - 1].history, 1, sizeof(m_scan_line[0][LINES_PER_ROTATION].history));
     memset(&m_scan_line[1][LINES_PER_ROTATION - 1].history, 1, sizeof(m_scan_line[1][LINES_PER_ROTATION].history));

     // last ones on 1 to display range circle    does not seem to work ???
    m_ptemp_icon = NULL;
    m_sent_bm_id_normal = -1;
    m_sent_bm_id_rollover =  -1;

    m_heading_source = HEADING_NONE;
    settings.auto_range_mode[0] = true;
    settings.auto_range_mode[1] = true;// starts with auto range change
    for (int i = 0; i < 2; i++){
        m_statistics[i].broken_packets = 0;
        m_statistics[i].broken_spokes = 0;
        m_statistics[i].missing_spokes = 0;
        m_statistics[i].packets = 0;
        m_statistics[i].spokes = 0;
    }
    data_seenAB[0] = 0;
    data_seenAB[1] = 0;
    m_pOptionsDialog = 0;
    m_pControlDialog = 0;
    m_pMessageBox = 0;
    m_pGuardZoneDialog = 0;
    m_pGuardZoneBogey = 0;
    m_pIdleDialog = 0;

    memset(&guardZones[0][0], 0, sizeof(guardZones));

    settings.guard_zone = 0;   // this used to be active guard zone, now it means which guard zone window is active
    settings.display_mode[0] = DM_CHART_OVERLAY;
    settings.display_mode[1] = DM_CHART_OVERLAY;
    settings.overlay_transparency = DEFAULT_OVERLAY_TRANSPARENCY;
    settings.refreshrate = 1;
    settings.timed_idle = 0;

    //      Set default parameters for controls displays
    m_BR24Controls_dialog_x = 0;    // position
    m_BR24Controls_dialog_y = 0;
    m_BR24Controls_dialog_sx = 200;  // size
    m_BR24Controls_dialog_sy = 200;

    m_GuardZoneBogey_x = 200;
    m_GuardZoneBogey_y = 200;

    ::wxDisplaySize(&m_display_width, &m_display_height);

    //****************************************************************************************
    //    Get a pointer to the opencpn configuration object
    m_pconfig = GetOCPNConfigObject();

    //    And load the configuration items
    if (LoadConfig()) {
        wxLogMessage(wxT("BR24radar_pi: Configuration file values initialised"));
        wxLogMessage(wxT("BR24radar_pi: Log verbosity = %d (to modify, set VerboseLog to 0..4)"), settings.verbose);
    }
    else {
        wxLogMessage(wxT("BR24radar_pi: configuration file values initialisation failed"));
        return 0; // give up
    }
    
    wxLongLong now = wxGetLocalTimeMillis();
    for (int i = 0; i < LINES_PER_ROTATION; i++) {
        m_scan_line[0][i].age = now - MAX_AGE * MILLISECONDS_PER_SECOND;
        m_scan_line[0][i].range = 0;
        m_scan_line[1][i].age = now - MAX_AGE * MILLISECONDS_PER_SECOND;
        m_scan_line[1][i].range = 0;
    }

    // Get a pointer to the opencpn display canvas, to use as a parent for the UI dialog
    m_parent_window = GetOCPNCanvasWindow();

    //    This PlugIn needs a toolbar icon

    m_tool_id  = InsertPlugInTool(wxT(""), _img_radar_red, _img_radar_red, wxITEM_NORMAL,
                                  wxT("BR24Radar"), wxT(""), NULL,
                                  BR24RADAR_TOOL_POSITION, 0, this);

    CacheSetToolbarToolBitmaps(BM_ID_RED, BM_ID_BLANK);

    //    Create the control socket for the Command Tx

    struct sockaddr_in adr;
    memset(&adr, 0, sizeof(adr));
    adr.sin_family = AF_INET;
    adr.sin_addr.s_addr=htonl(INADDR_ANY);
    adr.sin_port=htons(0);
    int one = 1;
    m_radar_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_radar_socket == INVALID_SOCKET) {
        r = -1;
    }
    else {
        r = setsockopt(m_radar_socket, SOL_SOCKET, SO_REUSEADDR, (const char *) &one, sizeof(one));
    }

    if (!r) {
        r = bind(m_radar_socket, (struct sockaddr *) &adr, sizeof(adr));
    }

    if (r) {
        wxLogError(wxT("BR24radar_pi: Unable to create UDP sending socket"));
        // Might as well give up now
        return 0;
    }

    // Context Menu Items (Right click on chart screen)

    m_pmenu = new wxMenu();            // this is a dummy menu
    // required by Windows as parent to item created
    wxMenuItem *pmi = new wxMenuItem(m_pmenu, -1, _("Radar Control..."));
#ifdef __WXMSW__
    wxFont *qFont = OCPNGetFont(_("Menu"), 10);
    pmi->SetFont(*qFont);
#endif
    br_radar_control_id = AddCanvasContextMenuItem(pmi, this);
    SetCanvasContextMenuItemViz(br_radar_control_id, true);


    wxMenuItem *pmi2 = new wxMenuItem(m_pmenu, -1, _("Set Guard Point"));
#ifdef __WXMSW__
    pmi->SetFont(*qFont);
#endif
    br_guard_zone_id = AddCanvasContextMenuItem(pmi2, this );
    SetCanvasContextMenuItemViz(br_guard_zone_id, false);
    br_guard_context_mode = false;

    //    Create the THREAD for Multicast radar data reception
    m_quit = false;

    m_reportReceiveThreadA = new RadarReportReceiveThread(this, &m_quit, 0);
    m_reportReceiveThreadA->Run();
    m_dataReceiveThreadA = new RadarDataReceiveThread(this, &m_quit, 0);
    m_dataReceiveThreadA->Run();
    m_commandReceiveThreadA = 0;
    m_commandReceiveThreadA = new RadarCommandReceiveThread(this, &m_quit, 0);
    m_commandReceiveThreadA->Run();
   

    m_dataReceiveThreadB = new RadarDataReceiveThread(this, &m_quit, 1);
    m_dataReceiveThreadB->Run();
    m_commandReceiveThreadB = 0;
    m_commandReceiveThreadB = new RadarCommandReceiveThread(this, &m_quit, 1);
    m_commandReceiveThreadB->Run();
    m_reportReceiveThreadB = new RadarReportReceiveThread(this, &m_quit, 1);
    m_reportReceiveThreadB->Run();
    
    ShowRadarControl(false);   //prepare radar control but don't show it
    control_box_closed = false; 
    control_box_opened = false;

    return (WANTS_DYNAMIC_OPENGL_OVERLAY_CALLBACK |
            WANTS_OPENGL_OVERLAY_CALLBACK |
            WANTS_OVERLAY_CALLBACK     |
            WANTS_CURSOR_LATLON        |
            WANTS_TOOLBAR_CALLBACK     |
            INSTALLS_TOOLBAR_TOOL      |
            INSTALLS_CONTEXTMENU_ITEMS |
            WANTS_CONFIG               |
            WANTS_NMEA_EVENTS          |
            WANTS_NMEA_SENTENCES       |
            WANTS_PREFERENCES          |
            WANTS_PLUGIN_MESSAGING
            );

}

bool br24radar_pi::DeInit(void)
{
    SaveConfig();
    m_quit = true; // Signal quit to any of the threads. Takes up to 1s.
    if (m_dataReceiveThreadA) {
        m_dataReceiveThreadA->Wait();
        delete m_dataReceiveThreadA;
    }
    
    if (m_dataReceiveThreadB) {
        m_dataReceiveThreadB->Wait();
        delete m_dataReceiveThreadB;
        wxLogMessage(wxT("BR24radar_pi: m_dataReceiveThreadA stopped in DeInit"));
    }
    
    if (m_commandReceiveThreadA) {
        m_commandReceiveThreadA->Wait();
        delete m_commandReceiveThreadA;
        wxLogMessage(wxT("BR24radar_pi: m_dataReceiveThreadB stopped in DeInit"));
    }
    
    if (m_commandReceiveThreadB) {
        m_commandReceiveThreadB->Wait();
        delete m_commandReceiveThreadB;
        wxLogMessage(wxT("BR24radar_pi: m_commandReceiveThreadA stopped in DeInit"));
    }
    
    if (m_reportReceiveThreadA) {
        m_reportReceiveThreadA->Wait();
        delete m_reportReceiveThreadA;
        wxLogMessage(wxT("BR24radar_pi: m_commandReceiveThreadB stopped in DeInit"));
    }
    
    if (m_reportReceiveThreadB) {
        m_reportReceiveThreadB->Wait();
        delete m_reportReceiveThreadB;
        wxLogMessage(wxT("BR24radar_pi: m_reportReceiveThreadA stopped in DeInit"));
    }
    
    if (m_radar_socket != INVALID_SOCKET) {
        closesocket(m_radar_socket);
        wxLogMessage(wxT("BR24radar_pi: m_reportReceiveThreadB stopped in DeInit"));
    }

    // I think we need to destroy any windows here
    OnBR24ControlDialogClose();
    OnBR24MessageBoxClose();

    return true;
}

int br24radar_pi::GetAPIVersionMajor()
{
    return MY_API_VERSION_MAJOR;
}

int br24radar_pi::GetAPIVersionMinor()
{
    return MY_API_VERSION_MINOR;
}

int br24radar_pi::GetPlugInVersionMajor()
{
    return PLUGIN_VERSION_MAJOR;
}

int br24radar_pi::GetPlugInVersionMinor()
{
    return PLUGIN_VERSION_MINOR;
}

wxBitmap *br24radar_pi::GetPlugInBitmap()
{
    return m_pdeficon;
}

wxString br24radar_pi::GetCommonName()
{
    return wxT("BR24Radar");
}


wxString br24radar_pi::GetShortDescription()
{
    return _("Navico Radar PlugIn for OpenCPN");
}


wxString br24radar_pi::GetLongDescription()
{
    return _("Navico Broadband BR24/3G/4G Radar PlugIn for OpenCPN\n");
}

void br24radar_pi::SetDefaults(void)
{
    // This will be called upon enabling a PlugIn via the user Dialog.
    // We don't need to do anything special here.
}

void br24radar_pi::ShowPreferencesDialog(wxWindow* parent)
{
    m_pOptionsDialog = new BR24DisplayOptionsDialog;
    m_pOptionsDialog->Create(m_parent_window, this);
    m_pOptionsDialog->ShowModal();
}

void logBinaryData(const wxString& what, const UINT8 * data, int size)
{
    wxString explain;
    int i = 0;

    explain.Alloc(size * 3 + 50);
    explain += wxT("BR24radar_pi: ");
    explain += what;
    explain += wxString::Format(wxT(" %d bytes: "), size);
    for (i = 0; i < size; i++) {
        explain += wxString::Format(wxT(" %02X"), data[i]);
    }
    wxLogMessage(explain);
}

//*********************************************************************************
// Display Preferences Dialog
//*********************************************************************************
IMPLEMENT_CLASS(BR24DisplayOptionsDialog, wxDialog)

BEGIN_EVENT_TABLE(BR24DisplayOptionsDialog, wxDialog)

EVT_CLOSE(BR24DisplayOptionsDialog::OnClose)
EVT_BUTTON(ID_OK, BR24DisplayOptionsDialog::OnIdOKClick)
EVT_RADIOBUTTON(ID_DISPLAYTYPE, BR24DisplayOptionsDialog::OnDisplayModeClick)
EVT_RADIOBUTTON(ID_OVERLAYDISPLAYOPTION, BR24DisplayOptionsDialog::OnDisplayOptionClick)

END_EVENT_TABLE()

BR24DisplayOptionsDialog::BR24DisplayOptionsDialog()
{
    Init();
}

BR24DisplayOptionsDialog::~BR24DisplayOptionsDialog()
{
}

void BR24DisplayOptionsDialog::Init()
{
}

bool BR24DisplayOptionsDialog::Create(wxWindow *parent, br24radar_pi *ppi)
{
    wxString m_temp;

    pParent = parent;
    pPlugIn = ppi;

    if (!wxDialog::Create(parent, wxID_ANY, _("BR24 Target Display Preferences"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE)) {
        return false;
    }

    int font_size_y, font_descent, font_lead;
    GetTextExtent( _T("0"), NULL, &font_size_y, &font_descent, &font_lead );
    wxSize small_button_size( -1, (int) ( 1.4 * ( font_size_y + font_descent + font_lead ) ) );

    int border_size = 4;
    wxBoxSizer* topSizer = new wxBoxSizer(wxVERTICAL);
    SetSizer(topSizer);

    wxFlexGridSizer * DisplayOptionsBox = new wxFlexGridSizer(2, 5, 5);
    topSizer->Add(DisplayOptionsBox, 0, wxALIGN_CENTER_HORIZONTAL | wxALL | wxEXPAND, 2);

    //  BR24 toolbox icon checkbox
    //    wxStaticBox* DisplayOptionsCheckBox = new wxStaticBox(this, wxID_ANY, _T(""));
    //    wxStaticBoxSizer* DisplayOptionsCheckBoxSizer = new wxStaticBoxSizer(DisplayOptionsCheckBox, wxVERTICAL);
    //    DisplayOptionsBox->Add(DisplayOptionsCheckBoxSizer, 0, wxEXPAND | wxALL, border_size);

    //  Range Units options

    wxString RangeModeStrings[] = {
        _("Nautical Miles"),
        _("Kilometers"),
    };

    pRangeUnits = new wxRadioBox(this, ID_RANGE_UNITS, _("Range Units"),
                                 wxDefaultPosition, wxDefaultSize,
                                 2, RangeModeStrings, 1, wxRA_SPECIFY_COLS);
    DisplayOptionsBox->Add(pRangeUnits, 0, wxALL | wxEXPAND, 2);

    pRangeUnits->Connect(wxEVT_COMMAND_RADIOBOX_SELECTED,
                         wxCommandEventHandler(BR24DisplayOptionsDialog::OnRangeUnitsClick), NULL, this);

    pRangeUnits->SetSelection(pPlugIn->settings.range_units);

    /// Option settings
    wxString Overlay_Display_Options[] = {
        _("Monocolor-Red"),
        _("Multi-color"),
        _("Multi-color 2"),
    };
    
    pOverlayDisplayOptions = new wxRadioBox(this, ID_OVERLAYDISPLAYOPTION, _("Overlay Display Options"),
                                            wxDefaultPosition, wxDefaultSize,
                                            3, Overlay_Display_Options, 1, wxRA_SPECIFY_COLS);

    DisplayOptionsBox->Add(pOverlayDisplayOptions, 0, wxALL | wxEXPAND, 2);

    pOverlayDisplayOptions->Connect(wxEVT_COMMAND_RADIOBOX_SELECTED,
                                    wxCommandEventHandler(BR24DisplayOptionsDialog::OnDisplayOptionClick), NULL, this);

    pOverlayDisplayOptions->SetSelection(pPlugIn->settings.display_option);
    
    /*
    pDisplayMode = new wxRadioBox(this, ID_DISPLAYTYPE, _("Radar Display"),
                                  wxDefaultPosition, wxDefaultSize,
                                  ARRAY_SIZE(DisplayModeStrings), DisplayModeStrings, 1, wxRA_SPECIFY_COLS);

    DisplayOptionsBox->Add(pDisplayMode, 0, wxALL | wxEXPAND, 2);

    pDisplayMode->Connect(wxEVT_COMMAND_RADIOBOX_SELECTED,
                          wxCommandEventHandler(BR24DisplayOptionsDialog::OnDisplayModeClick), NULL, this);

    pDisplayMode->SetSelection(pPlugIn->settings.display_mode[0]);
    */
    wxString GuardZoneStyleStrings[] = {
        _("Shading"),
        _("Outline"),
        _("Shading + Outline"),
    };
    pGuardZoneStyle = new wxRadioBox(this, ID_DISPLAYTYPE, _("Guard Zone Styling"),
                                     wxDefaultPosition, wxDefaultSize,
                                     3, GuardZoneStyleStrings, 1, wxRA_SPECIFY_COLS);

    DisplayOptionsBox->Add(pGuardZoneStyle, 0, wxALL | wxEXPAND, 2);
    pGuardZoneStyle->Connect(wxEVT_COMMAND_RADIOBOX_SELECTED,
                             wxCommandEventHandler(BR24DisplayOptionsDialog::OnGuardZoneStyleClick), NULL, this);
    pGuardZoneStyle->SetSelection(pPlugIn->settings.guard_zone_render_style);


    //  Calibration
    wxStaticBox* itemStaticBoxCalibration = new wxStaticBox(this, wxID_ANY, _("Calibration"));
    wxStaticBoxSizer* itemStaticBoxSizerCalibration = new wxStaticBoxSizer(itemStaticBoxCalibration, wxVERTICAL);
    DisplayOptionsBox->Add(itemStaticBoxSizerCalibration, 0, wxEXPAND | wxALL, border_size);

    // Heading correction
    wxStaticText *pStatic_Heading_Correction = new wxStaticText(this, wxID_ANY, _("Heading correction\n(-180 to +180)"));
    itemStaticBoxSizerCalibration->Add(pStatic_Heading_Correction, 1, wxALIGN_LEFT | wxALL, 2);

    pText_Heading_Correction_Value = new wxTextCtrl(this, wxID_ANY);
    itemStaticBoxSizerCalibration->Add(pText_Heading_Correction_Value, 1, wxALIGN_LEFT | wxALL, border_size);
    m_temp.Printf(wxT("%2.1f"), pPlugIn->settings.heading_correction);
    pText_Heading_Correction_Value->SetValue(m_temp);
    pText_Heading_Correction_Value->Connect(wxEVT_COMMAND_TEXT_UPDATED,
                                            wxCommandEventHandler(BR24DisplayOptionsDialog::OnHeading_Calibration_Value), NULL, this);

    // Guard Zone Alarm

    wxStaticBox* guardZoneBox = new wxStaticBox(this, wxID_ANY, _("Guard Zone Sound"));
    wxStaticBoxSizer* guardZoneSizer = new wxStaticBoxSizer(guardZoneBox, wxVERTICAL);
    DisplayOptionsBox->Add(guardZoneSizer, 0, wxEXPAND | wxALL, border_size);

    wxButton *pSelectSound = new wxButton(this, ID_SELECT_SOUND, _("Select Alert Sound"), wxDefaultPosition, small_button_size, 0);
    pSelectSound->Connect(wxEVT_COMMAND_BUTTON_CLICKED,
                          wxCommandEventHandler(BR24DisplayOptionsDialog::OnSelectSoundClick), NULL, this);
    guardZoneSizer->Add(pSelectSound, 0, wxALL, border_size);

    wxButton *pTestSound = new wxButton(this, ID_TEST_SOUND, _("Test Alert Sound"), wxDefaultPosition, small_button_size, 0);
    pTestSound->Connect(wxEVT_COMMAND_BUTTON_CLICKED,
                        wxCommandEventHandler(BR24DisplayOptionsDialog::OnTestSoundClick), NULL, this);
    guardZoneSizer->Add(pTestSound, 0, wxALL, border_size);

    //  Options
    wxStaticBox* itemStaticBoxOptions = new wxStaticBox(this, wxID_ANY, _("Options"));
    wxStaticBoxSizer* itemStaticBoxSizerOptions = new wxStaticBoxSizer(itemStaticBoxOptions, wxVERTICAL);
    topSizer->Add(itemStaticBoxSizerOptions, 0, wxEXPAND | wxALL, border_size);

    cbPassHeading = new wxCheckBox(this, ID_PASS_HEADING, _("Pass radar heading to OpenCPN"), wxDefaultPosition, wxDefaultSize, wxALIGN_CENTRE | wxST_NO_AUTORESIZE);
    itemStaticBoxSizerOptions->Add(cbPassHeading, 0, wxALIGN_CENTER_VERTICAL | wxALL, border_size);
    cbPassHeading->SetValue(pPlugIn->settings.passHeadingToOCPN ? true : false);
    cbPassHeading->Connect(wxEVT_COMMAND_CHECKBOX_CLICKED,
                             wxCommandEventHandler(BR24DisplayOptionsDialog::OnPassHeadingClick), NULL, this);

    cbEnableDualRadar = new wxCheckBox(this, ID_SELECT_AB, _("Enable dual radar, 4G only"), wxDefaultPosition, wxDefaultSize, wxALIGN_CENTRE | wxST_NO_AUTORESIZE);
    itemStaticBoxSizerOptions->Add(cbEnableDualRadar, 0, wxALIGN_CENTER_VERTICAL | wxALL, border_size);
    cbEnableDualRadar->SetValue(pPlugIn->settings.enable_dual_radar ? true : false);
    cbEnableDualRadar->Connect(wxEVT_COMMAND_CHECKBOX_CLICKED,
        wxCommandEventHandler(BR24DisplayOptionsDialog::OnEnableDualRadarClick), NULL, this);

    cbEmulator = new wxCheckBox(this, ID_EMULATOR, _("Emulator mode"), wxDefaultPosition, wxDefaultSize, wxALIGN_CENTRE | wxST_NO_AUTORESIZE);
    itemStaticBoxSizerOptions->Add(cbEmulator, 0, wxALIGN_CENTER_VERTICAL | wxALL, border_size);
    cbEmulator->SetValue(pPlugIn->settings.emulator_on ? true : false);
    cbEmulator->Connect(wxEVT_COMMAND_CHECKBOX_CLICKED,
        wxCommandEventHandler(BR24DisplayOptionsDialog::OnEmulatorClick), NULL, this);


  
    // Accept/Reject button
    wxStdDialogButtonSizer* DialogButtonSizer = wxDialog::CreateStdDialogButtonSizer(wxOK | wxCANCEL);
    topSizer->Add(DialogButtonSizer, 0, wxALIGN_RIGHT | wxALL, border_size);

    DimeWindow(this);

    Fit();
    SetMinSize(GetBestSize());

    return true;
}

void BR24DisplayOptionsDialog::OnRangeUnitsClick(wxCommandEvent &event)
{
    pPlugIn->settings.range_units = pRangeUnits->GetSelection();
}

void BR24DisplayOptionsDialog::OnDisplayOptionClick(wxCommandEvent &event)
{
    pPlugIn->settings.display_option = pOverlayDisplayOptions->GetSelection();
}

void BR24DisplayOptionsDialog::OnDisplayModeClick(wxCommandEvent &event)
{
    pPlugIn->SetDisplayMode((DisplayModeType) pDisplayMode->GetSelection());
}

void BR24DisplayOptionsDialog::OnGuardZoneStyleClick(wxCommandEvent &event)
{
    pPlugIn->settings.guard_zone_render_style = pGuardZoneStyle->GetSelection();
}

void BR24DisplayOptionsDialog::OnSelectSoundClick(wxCommandEvent &event)
{
    wxString *sharedData = GetpSharedDataLocation();
    wxString sound_dir;

    sound_dir.Append( *sharedData );
    sound_dir.Append( wxT("sounds") );

    wxFileDialog *openDialog = new wxFileDialog( NULL, _("Select Sound File"), sound_dir, wxT(""),
                                                _("WAV files (*.wav)|*.wav|All files (*.*)|*.*"), wxFD_OPEN );
    int response = openDialog->ShowModal();
    if (response == wxID_OK ) {
        pPlugIn->settings.alert_audio_file = openDialog->GetPath();
    }
}

void BR24DisplayOptionsDialog::OnEnableDualRadarClick(wxCommandEvent &event)
{
    pPlugIn->settings.enable_dual_radar = cbEnableDualRadar->GetValue();
}

void BR24DisplayOptionsDialog::OnTestSoundClick(wxCommandEvent &event)
{
    if (!pPlugIn->settings.alert_audio_file.IsEmpty()) {
        PlugInPlaySound(pPlugIn->settings.alert_audio_file);
    }
}

void BR24DisplayOptionsDialog::OnHeading_Calibration_Value(wxCommandEvent &event)
{
    wxString temp = pText_Heading_Correction_Value->GetValue();
    temp.ToDouble(&pPlugIn->settings.heading_correction);
}

void BR24DisplayOptionsDialog::OnPassHeadingClick(wxCommandEvent &event)
{
    pPlugIn->settings.passHeadingToOCPN = cbPassHeading->GetValue();
}

void BR24DisplayOptionsDialog::OnEmulatorClick(wxCommandEvent &event)
{
    pPlugIn->settings.emulator_on = cbEmulator->GetValue();
}


void BR24DisplayOptionsDialog::OnClose(wxCloseEvent& event)
{
    pPlugIn->SaveConfig();
    this->Hide();
}

void BR24DisplayOptionsDialog::OnIdOKClick(wxCommandEvent& event)
{
    pPlugIn->SaveConfig();
    this->Hide();
}

//********************************************************************************
// Operation Dialogs - Control, Manual, and Options

void br24radar_pi::ShowRadarControl(bool show)
{
    if (!m_pMessageBox) {
        m_pMessageBox = new BR24MessageBox;
        m_pMessageBox->Create(m_parent_window, this);
        m_pMessageBox->SetSize(m_BR24Message_box_x, m_BR24Message_box_y,
            m_BR24Message_box_sx, m_BR24Message_box_sy);
        m_pMessageBox->Fit();
    }
    m_pMessageBox->Hide();

    if (!m_pControlDialog) {
        m_pControlDialog = new BR24ControlsDialog;
        m_pControlDialog->Create(m_parent_window, this);
        m_pControlDialog->SetSize(m_BR24Controls_dialog_x, m_BR24Controls_dialog_y,
            m_BR24Controls_dialog_sx, m_BR24Controls_dialog_sy);
        if (br_radar_type == RT_BR24){
            m_pControlDialog->bRadarAB->Hide();
        }
        m_pControlDialog->Fit();
        m_pControlDialog->Hide();
        int range = br_range_meters[settings.selectRadarB];
        int idx = convertMetersToRadarAllowedValue(&range, settings.range_units, br_radar_type);
        m_pControlDialog->SetRangeIndex(idx);
        radar_setting[settings.selectRadarB].range.Update(idx);
        m_pControlDialog->Hide();
    }
    
    m_pControlDialog->UpdateControl(br_opengl_mode
        , br_bpos_set
        , m_heading_source != HEADING_NONE
        , br_var_source != VARIATION_SOURCE_NONE
        , br_radar_seen
        , br_data_seen
        );
    m_pControlDialog->UpdateControlValues(true);
    m_pMessageBox->UpdateMessage(br_opengl_mode
        , br_bpos_set
        , m_heading_source != HEADING_NONE
        , br_var_source != VARIATION_SOURCE_NONE
        , br_radar_seen
        , br_data_seen
        );
    control_box_closed = false;
}

void br24radar_pi::OnContextMenuItemCallback(int id)
{
    if (!br_guard_context_mode) {
        ShowRadarControl(true);
        control_box_closed = false;
        control_box_opened = true;
    }

    if (br_guard_context_mode) {
        SetCanvasContextMenuItemViz(br_radar_control_id, false);
        br_mark_rng = local_distance(br_ownship_lat, br_ownship_lon, br_cur_lat, br_cur_lon);
        br_mark_brg = local_bearing(br_ownship_lat, br_ownship_lon, br_cur_lat, br_cur_lon);
        m_pGuardZoneDialog->OnContextMenuGuardCallback(br_mark_rng, br_mark_brg);
    }
}

void br24radar_pi::OnBR24ControlDialogClose()
{
    if (m_pControlDialog) {
        m_pControlDialog->GetPosition(&m_BR24Controls_dialog_x, &m_BR24Controls_dialog_y);
        m_pControlDialog->Hide();
        SetCanvasContextMenuItemViz(br_guard_zone_id, false);
        control_box_closed = true;
        control_box_opened = false;
    }
    SaveConfig();
}

void br24radar_pi::OnBR24MessageBoxClose()
{
    if (m_pMessageBox) {
        m_pMessageBox->GetPosition(&m_BR24Message_box_x, &m_BR24Message_box_y);
        m_pMessageBox->Hide();
    }
    SaveConfig();
}

void br24radar_pi::OnGuardZoneDialogClose()
{
    if (m_pGuardZoneDialog) {
        m_pGuardZoneDialog->GetPosition(&m_BR24Controls_dialog_x, &m_BR24Controls_dialog_y);
        m_pGuardZoneDialog->Hide();
        SetCanvasContextMenuItemViz(br_guard_zone_id, false);
        br_guard_context_mode = false;
        br_guard_bogey_confirmed = false;
        SaveConfig();
    }
    if (m_pControlDialog) {
        m_pControlDialog->UpdateGuardZoneState();
        if (!control_box_closed) {
            m_pControlDialog->Show();
        }
        m_pControlDialog->SetPosition(wxPoint(m_BR24Controls_dialog_x, m_BR24Controls_dialog_y));
        SetCanvasContextMenuItemViz(br_radar_control_id, true);
    }

}

void br24radar_pi::OnGuardZoneBogeyConfirm()
{
    br_guard_bogey_confirmed = true; // This will stop the sound being repeated
}

void br24radar_pi::OnGuardZoneBogeyClose()
{
    br_guard_bogey_confirmed = true; // This will stop the sound being repeated
    if (m_pGuardZoneBogey) {
        m_pGuardZoneBogey->GetPosition(&m_GuardZoneBogey_x, &m_GuardZoneBogey_y);
        m_pGuardZoneBogey->Hide();
        // x        SaveConfig();
    }
}

void br24radar_pi::Select_Guard_Zones(int zone)
{
    settings.guard_zone = zone;

    if (!m_pGuardZoneDialog) {
        m_pGuardZoneDialog = new GuardZoneDialog;
        wxPoint pos = wxPoint(m_BR24Controls_dialog_x, m_BR24Controls_dialog_y); // show at same loc as controls
        m_pGuardZoneDialog->Create(m_parent_window, this, wxID_ANY, _(" Guard Zone Control"), pos);
    }
    if (zone >= 0) {
        m_pControlDialog->GetPosition(&m_BR24Controls_dialog_x, &m_BR24Controls_dialog_y);
        m_pGuardZoneDialog->Show();
        m_pControlDialog->Hide();
        m_pGuardZoneDialog->SetPosition(wxPoint(m_BR24Controls_dialog_x, m_BR24Controls_dialog_y));
        m_pGuardZoneDialog->OnGuardZoneDialogShow(zone);
        SetCanvasContextMenuItemViz(br_guard_zone_id, true);
        SetCanvasContextMenuItemViz(br_radar_control_id, false);
        br_guard_context_mode = true;
    }
    else {
        m_pGuardZoneDialog->Hide();
        SetCanvasContextMenuItemViz(br_guard_zone_id, false);
        SetCanvasContextMenuItemViz(br_radar_control_id, true);
        br_guard_context_mode = false;
    }
}

void br24radar_pi::SetDisplayMode(DisplayModeType mode)
{
    settings.display_mode[settings.selectRadarB] = mode;
}

long br24radar_pi::GetRangeMeters()
{
    return (long) br_range_meters[settings.selectRadarB];
}

void br24radar_pi::UpdateDisplayParameters(void)
{
    RequestRefresh(GetOCPNCanvasWindow());
}

//*******************************************************************************
// ToolBar Actions

int br24radar_pi::GetToolbarToolCount(void)
{
    return 1;
}

void br24radar_pi::OnToolbarToolCallback(int id)
{
    if (toolbar_button == RED) {
        // radar is off (not seen), but obviously we want to see it
        if (settings.showRadar == false){
            settings.showRadar = true;  // but we don't send the transmit command, no use as radar is off
        }
        else{
            settings.showRadar = false;  // toggle showRadar on RED
        }
    }
    else if (toolbar_button == AMBER){
        settings.showRadar = true;   // switch radar on and show it
        if (!data_seenAB[settings.selectRadarB]) {
            RadarTxOn();
        }
        if (id != 999999 && settings.timed_idle != 0) {
            m_pControlDialog->SetTimedIdleIndex(0); // Disable Timed Transmit if user click the icon while idle
        }
        if (settings.verbose) {
        }
        ShowRadarControl(true);
    }
    else if (toolbar_button == GREEN){
        if (id == 999 && settings.timed_idle != 0) { // Disable Timed Transmit
            m_pControlDialog->SetTimedIdleIndex(0);
            return;
        }
        settings.showRadar = 0;
        RadarTxOff();
        if (id != 999999 && settings.timed_idle != 0) {
            m_pControlDialog->SetTimedIdleIndex(0); // Disable Timed Transmit if user click the icon while idle
        }
        OnGuardZoneDialogClose();
        if (m_pControlDialog) {
            m_pControlDialog->UpdateControl(br_opengl_mode
                , br_bpos_set
                , m_heading_source != HEADING_NONE
                , br_var_source != VARIATION_SOURCE_NONE
                , br_radar_seen
                , br_data_seen
                );
        }
        if (m_pMessageBox) {
            m_pMessageBox->UpdateMessage(br_opengl_mode
                , br_bpos_set
                , m_heading_source != HEADING_NONE
                , br_var_source != VARIATION_SOURCE_NONE
                , br_radar_seen
                , br_data_seen
                );
        }
        if (m_pGuardZoneBogey) {
            m_pGuardZoneBogey->Hide();
        }
        UpdateState();
    }
}

  

// DoTick
// ------
// Called on every RenderGLOverlay call, i.e. once a second.
//
// This checks if we need to ping the radar to keep it alive (or make it alive)
//*********************************************************************************
// Keeps Radar scanner on line if master and radar on -  run by RenderGLOverlay

void br24radar_pi::DoTick(void)
{
    if (settings.verbose){
        static time_t refresh_indicater = 0;
        static int performance_counter = 0;
        performance_counter++;
        int timer = time(0) - refresh_indicater;
        if (time(0) - refresh_indicater >= 1){
            refresh_indicater = time(0);
            wxLogMessage(wxT("BR24radar_pi: number of refreshes last second = %d"), performance_counter);
            performance_counter = 0;
        }
    }
    time_t now = time(0);
    static time_t previousTicks = 0;
    if (now == previousTicks) {
        // Repeated call during scroll, do not do Tick processing
        return;
    }

    previousTicks = now;
    if (br_radar_type == RT_BR24){    // make sure radar A only.
        settings.selectRadarB = 0;
        settings.enable_dual_radar = 0;
    }

    br_refresh_rate = REFRESHMAPPING[settings.refreshrate - 1];  // set actual refresh rate

    if (br_bpos_set && TIMER_ELAPSED(br_bpos_watchdog)) {
        // If the position data is 10s old reset our heading.
        // Note that the watchdog is continuously reset every time we receive a heading.
        br_bpos_set = false;
        wxLogMessage(wxT("BR24radar_pi: Lost Boat Position data"));
    }

    if (m_heading_source != HEADING_NONE && TIMER_ELAPSED(br_hdt_watchdog)) {
        // If the position data is 10s old reset our heading.
        // Note that the watchdog is continuously reset every time we receive a heading
        m_heading_source = HEADING_NONE;
        wxLogMessage(wxT("BR24radar_pi: Lost Heading data"));
        if (m_pMessageBox) {
            if (m_pMessageBox->IsShown()) {
                wxString info = wxT("");
                m_pMessageBox->SetHeadingInfo(info);
            }
        }
    }

    if (br_var_source != VARIATION_SOURCE_NONE && TIMER_ELAPSED(br_var_watchdog)) {
        br_var_source = VARIATION_SOURCE_NONE;
        wxLogMessage(wxT("BR24radar_pi: Lost Variation source"));
        if (m_pMessageBox) {
            if (m_pMessageBox->IsShown()) {
                wxString info = wxT("");
                m_pMessageBox->SetVariationInfo(info);
            }
        }
    }

    if (br_radar_seen && TIMER_ELAPSED(br_radar_watchdog)) {
        br_radar_seen = false;
        previous_br_radar_seen = false;
        wxLogMessage(wxT("BR24radar_pi: Lost radar presence"));
    }
    if (!previous_br_radar_seen && br_radar_seen){
    
        if (RadarStayAlive()){      // send stay alive to obtain control blocks from radar
            previous_br_radar_seen = true;
        }
    }
//    wxLogMessage(wxT("BR24radar_pi: First radar data seen spokes %d, broken %d"), m_statistics[settings.selectRadarB].spokes, m_statistics[settings.selectRadarB].broken_spokes);
    data_seenAB[0] = m_statistics[0].spokes > m_statistics[0].broken_spokes;
    data_seenAB[1] = m_statistics[1].spokes > m_statistics[1].broken_spokes;
    if (data_seenAB[0] || data_seenAB[1]) { // Something coming from radar unit?
        br_data_seen = true;
        br_data_watchdog = now;
        if (br_scanner_state != RADAR_ON) {
            br_scanner_state = RADAR_ON;
        }
        if (settings.showRadar) {   // if not, radar will time out and go standby
            if (now - br_dt_stayalive >= STAYALIVE_TIMEOUT) {
                br_dt_stayalive = now;
                RadarStayAlive();
            }
        }
    }
    else {
        br_scanner_state = RADAR_OFF;
        if (br_data_seen && TIMER_ELAPSED(br_data_watchdog)) {
            br_heading_on_radar = false;
            br_data_seen = false;
            wxLogMessage(wxT("BR24radar_pi: Lost radar data"));
        }
    }

    if ((settings.passHeadingToOCPN && br_heading_on_radar)) {
        wxString nmeastring;
        //    if (blackout) br_hdt = 0;  // heads up in blackout mode
        nmeastring.Printf(_T("$APHDT,%05.1f,M\r\n"), br_hdt);
        PushNMEABuffer(nmeastring);
    }

    wxString t;
    t.Printf(wxT("packets %d/%d\nspokes %d/%d/%d")
        , m_statistics[settings.selectRadarB].packets
        , m_statistics[settings.selectRadarB].broken_packets
        , m_statistics[settings.selectRadarB].spokes
        , m_statistics[settings.selectRadarB].broken_spokes
        , m_statistics[settings.selectRadarB].missing_spokes);

    if (m_pMessageBox) {
        if (m_pMessageBox->IsShown()) {
            m_pMessageBox->SetRadarInfo(t);
        }
    }
    if (settings.verbose >= 1) {
        t.Replace(wxT("\n"), wxT(" "));
        if (settings.verbose) {
            wxLogMessage(wxT("BR24radar_pi: ReCeived %s, %d %d %d %d"), t.c_str(), br_bpos_set, m_heading_source, br_radar_seen, br_data_seen);
        }
    }
    if (m_pControlDialog) {
        m_pControlDialog->UpdateControl(br_opengl_mode
            , br_bpos_set
            , m_heading_source != HEADING_NONE
            , br_var_source != VARIATION_SOURCE_NONE
            , br_radar_seen
            , br_data_seen
            );
        m_pControlDialog->UpdateControlValues(false);
    }

    if (m_pMessageBox) {
        m_pMessageBox->UpdateMessage(br_opengl_mode
            , br_bpos_set
            , m_heading_source != HEADING_NONE
            , br_var_source != VARIATION_SOURCE_NONE
            , br_radar_seen
            , br_data_seen
            );
    }

    for (int i = 0; i < 2; i++){
        m_statistics[i].broken_packets = 0;
        m_statistics[i].broken_spokes = 0;
        m_statistics[i].missing_spokes = 0;
        m_statistics[i].packets = 0;
        m_statistics[i].spokes = 0;
    }

    if (settings.emulator_on){
        br_radar_seen = true;
        br_radar_watchdog = time(0);
        settings.selectRadarB = 0;
    }

    /*******************************************
     Function Timed Transmit. Check if active
     ********************************************/
    if (settings.timed_idle != 0 && toolbar_button) { //Reset function if radar connection is lost RED==0
        time_t TT_now = time(0);
        int factor = 5 * 60;
        if (br_init_timed_transmit) {   //Await user finalizing idle time option set.
            if (toolbar_button == GREEN) {
                TimedTransmit_IdleBoxMode = 2;
                if (TT_now > (br_idle_watchdog + (settings.idle_run_time * 60)) || br_idle_dialog_time_left == 999) {
                    RadarTxOff();                 //Stop radar scanning
                    settings.showRadar = 0;
                    br_idle_watchdog = TT_now;
                }
            }
            else if (toolbar_button == AMBER) {
                TimedTransmit_IdleBoxMode = 1;
                if (TT_now > (br_idle_watchdog + (settings.timed_idle * factor)) || br_idle_dialog_time_left == 999) {
                    br24radar_pi::OnToolbarToolCallback(999999);    //start radar scanning
                    br_idle_watchdog = TT_now;
                }
            }
            // Send minutes left to Idle dialog box            
            if (!m_pIdleDialog) {
                m_pIdleDialog = new Idle_Dialog;
                m_pIdleDialog->Create(m_parent_window, this);
            }
            if (TimedTransmit_IdleBoxMode == 1) {   //Idle
                time_left = ((br_idle_watchdog + (settings.timed_idle * factor)) - TT_now) / 60;
                if (br_idle_dialog_time_left != time_left) {
                    br24radar_pi::m_pIdleDialog->SetIdleTimes(TimedTransmit_IdleBoxMode, settings.timed_idle * factor / 60, time_left);
                    m_pIdleDialog->Show();
                    br_idle_dialog_time_left = time_left;
                }
            }
            if (TimedTransmit_IdleBoxMode == 2) {   //Transmit
                time_left = ((br_idle_watchdog + (settings.idle_run_time * 60)) - TT_now) / 60;
                if (br_idle_dialog_time_left != time_left) {
                       br24radar_pi::m_pIdleDialog->SetIdleTimes(TimedTransmit_IdleBoxMode, settings.idle_run_time, time_left);
                    m_pIdleDialog->Show();
                    br_idle_dialog_time_left = time_left;
                }
            }
        }
        else {
            if(m_pControlDialog) {
                if(m_pControlDialog->topSizer->IsShown(m_pControlDialog->controlBox)) {
                    br_init_timed_transmit = true;  //First time init: Await user to leave Timed transmit setting menu.
                    br_idle_watchdog = TT_now;
                }
            }
        }
    }
    else {
        if(br_init_timed_transmit) {
            br_idle_dialog_time_left = 999;
            if (m_pIdleDialog) m_pIdleDialog->Close();
            settings.timed_idle = 0;
            br_init_timed_transmit = false;
        }
    }   //End of Timed Transmit

    UpdateState();
}        // end of br24radar_pi::DoTick(void)

void br24radar_pi::UpdateState(void)   // -  run by RenderGLOverlay  updates the color of the toolbar button
{
    if (!br_radar_seen || !br_opengl_mode) {
        toolbar_button = RED;
        CacheSetToolbarToolBitmaps(BM_ID_RED, BM_ID_RED);
    }
    else if (br_data_seen && settings.showRadar) {
        toolbar_button = GREEN;
        CacheSetToolbarToolBitmaps(BM_ID_GREEN, BM_ID_GREEN);
    }
    else {
        toolbar_button = AMBER;
        CacheSetToolbarToolBitmaps(BM_ID_AMBER, BM_ID_AMBER);
    }
}


//***********************************************************************************************************
// Radar Image Graphic Display Processes
//***********************************************************************************************************

bool br24radar_pi::RenderOverlay(wxDC &dc, PlugIn_ViewPort *vp)
{
    br_opengl_mode = false;
    
    DoTick(); // update timers and watchdogs

    UpdateState(); // update the toolbar

    return true;
}

// Called by Plugin Manager on main system process cycle

bool br24radar_pi::RenderGLOverlay(wxGLContext *pcontext, PlugIn_ViewPort *vp)
{
    br_refresh_busy_or_queued = true;   //  the receive thread should not queue another refresh (through refresh canvas) this when it is busy
    br_opengl_mode = true;
    // this is expected to be called at least once per second
    // but if we are scrolling or otherwise it can be MUCH more often!

    // Always compute br_auto_range_meters, possibly needed by SendState() called from DoTick().
    double max_distance = radar_distance(vp->lat_min, vp->lon_min, vp->lat_max, vp->lon_max, 'm');
    // max_distance is the length of the diagonal of the viewport. If the boat were centered, the
    // max length to the edge of the screen is exactly half that.
    double edge_distance = max_distance / 2.0;
    br_auto_range_meters = (int) edge_distance;
    if (br_auto_range_meters < 50)
    {
      br_auto_range_meters = 50;
    }
    blackout[settings.selectRadarB] = settings.showRadar == 1 && br_data_seen && settings.display_mode[settings.selectRadarB] == DM_CHART_BLACKOUT;
    DoTick(); // update timers and watchdogs
    UpdateState(); // update the toolbar
    wxPoint center_screen(vp->pix_width / 2, vp->pix_height / 2);
    wxPoint boat_center, pp;
    if (br_bpos_set) {
        GetCanvasPixLL(vp, &pp, br_ownship_lat, br_ownship_lon);
        boat_center = pp;
        gLat = br_ownship_lat;
        gLon = br_ownship_lon;
    } else {
        GetCanvasPixLL(vp, &pp, gLat, gLon);
        boat_center = pp;
     //   boat_center = center_screen;
    }

    // set the IP address info in the control box if signalled by the receive thread

    if (br_update_error_control) {
        if (m_pMessageBox) {
            if (m_pMessageBox->IsShown()) {
                m_pMessageBox->SetErrorMessage(br_error_msg);
            }
        }
        br_update_error_control = false;
    }
    if (br_update_address_control) {
        if (m_pMessageBox) {
            if (m_pMessageBox->IsShown()) {
                m_pMessageBox->SetMcastIPAddress(br_ip_address);
            }
        }
        br_update_address_control = false;
    }


    // now set a new value in the range control if an unsollicited range change has been received.
    // not for range change that the pi has initialized. For these the control was updated immediately

    if (br_update_range_control[settings.selectRadarB]) {
        br_update_range_control[settings.selectRadarB] = false;
        int radar_range = br_range_meters[settings.selectRadarB];
        int idx = convertRadarMetersToIndex(&radar_range, settings.range_units, br_radar_type);
        radar_setting[settings.selectRadarB].range.Update(idx);
        // above also updates radar_range to be a display value (lower, rounded number)
        if (m_pControlDialog) {
            if (radar_range != br_commanded_range_meters) { // this range change was not initiated by the pi
                m_pControlDialog->SetRemoteRangeIndex(idx);
                if (settings.verbose) {
                    wxLogMessage(wxT("BR24radar_pi: remote range change to %d meters = %d (plugin commanded %d meters)"), br_range_meters[settings.selectRadarB], radar_range, br_commanded_range_meters);
                }
            }
            else {
                m_pControlDialog->SetRangeIndex(idx);
                if (settings.verbose) {
                    wxLogMessage(wxT("BR24radar_pi: final range change to %d meters = %d"), br_range_meters[settings.selectRadarB], radar_range);
                }
            }
        }
    }


    // Calculate the "optimum" radar range setting in meters so the radar image just fills the screen

    if (settings.auto_range_mode[settings.selectRadarB] && settings.showRadar) {
        // Don't adjust auto range meters continuously when it is oscillating a little bit (< 5%)
        // This also prevents the radar from issuing a range command after a remote range change
        int test = 100 * br_previous_auto_range_meters / br_auto_range_meters;
        if (test < 95 || test > 105) { //   range change required
            if (settings.verbose) {
                wxLogMessage(wxT("BR24radar_pi: Automatic range changed from %d to %d meters")
                             , br_previous_auto_range_meters, br_auto_range_meters);
            }
            br_previous_auto_range_meters = br_auto_range_meters;
            // Compute a 'standard' distance. This will be slightly smaller.
            int displayedRange = br_auto_range_meters;
            size_t idx = convertMetersToRadarAllowedValue(&displayedRange, settings.range_units, br_radar_type);
            if (displayedRange != br_commanded_range_meters) {
                if (m_pControlDialog) {
                    m_pControlDialog->SetRangeIndex(idx);
                }
                SetRangeMeters(displayedRange);
            }
        }
    }


    //    Calculate image scale factor

    GetCanvasLLPix(vp, wxPoint(0, vp->pix_height-1), &ulat, &ulon);  // is pix_height a mapable coordinate?
    GetCanvasLLPix(vp, wxPoint(0, 0), &llat, &llon);
    dist_y = radar_distance(llat, llon, ulat, ulon, 'm'); // Distance of height of display - meters
    pix_y = vp->pix_height;
    v_scale_ppm = 1.0;
    if (dist_y > 0.) {
        // v_scale_ppm = vertical pixels per meter
        v_scale_ppm = vp->pix_height / dist_y ;    // pixel height of screen div by equivalent meters
    }

    switch (settings.display_mode[settings.selectRadarB]) {
        case DM_CHART_OVERLAY:
        case DM_CHART_BLACKOUT:
            RenderRadarOverlay(boat_center, v_scale_ppm, vp);
            break;
    }
    br_refresh_busy_or_queued = false;
    return true;
}

void br24radar_pi::RenderRadarOverlay(wxPoint radar_center, double v_scale_ppm, PlugIn_ViewPort *vp)
{
    glPushAttrib(GL_COLOR_BUFFER_BIT | GL_LINE_BIT | GL_HINT_BIT);      //Save state
    blackout[settings.selectRadarB] = settings.showRadar == 1 && br_data_seen && settings.display_mode[settings.selectRadarB] == DM_CHART_BLACKOUT;
                //  radar only mode, will be head up, operate also without heading or position
    if (!blackout[settings.selectRadarB]) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }
    else {
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    glPushMatrix();
    glTranslated(radar_center.x, radar_center.y, 0);

    if (settings.verbose >= 4) {
        wxLogMessage(wxT("BR24radar_pi: RenderRadarOverlay lat=%g lon=%g v_scale_ppm=%g rotation=%g skew=%g scale=%f")
                    , vp->clat
                    , vp->clon
                    , vp->view_scale_ppm
                    , vp->rotation
                    , vp->skew
                    , vp->chart_scale
                    );
    }

    double heading = MOD_DEGREES( rad2deg(vp->rotation)        // viewport rotation
                                + 270.0                        // difference between compass and OpenGL rotation
                                + settings.heading_correction  // fix any radome rotation fault
                                - vp->skew * settings.skew_factor
                                );
    glRotatef(heading, 0, 0, 1);

    // scaling...
    int meters = br_range_meters[settings.selectRadarB];
    if (!meters) meters = br_auto_range_meters;
    if (!meters) meters = 1000;
    double radar_pixels_per_meter = ((double) RETURNS_PER_LINE) / meters;
    double scale_factor =  v_scale_ppm / radar_pixels_per_meter;  // screen pix/radar pix

//    if (settings.showRadar && ((br_bpos_set && m_heading_source != HEADING_NONE) || settings.display_mode[settings.selectRadarB] == DM_EMULATOR 
//        || blackout[settings.selectRadarB])) {
    if (blackout[settings.selectRadarB] || (settings.showRadar == 1 && br_bpos_set && m_heading_source != HEADING_NONE && br_data_seen) ||
        (settings.emulator_on && settings.showRadar)){
        glPushMatrix();
        glScaled(scale_factor, scale_factor, 1.);
        if (br_range_meters[settings.selectRadarB] > 0 && br_scanner_state == RADAR_ON) {
            // Guard Section
            for (int i = 0; i < 4; i++){
                bogey_count[i] = 0;
            }
            static int metersA, metersB;
            if (settings.selectRadarB == 0) metersA = meters;
            if (settings.selectRadarB == 1) metersB = meters;
            if (settings.showRadar == RADAR_ON && metersA != 0){
                Guard(metersA, 0);
            }
            if (settings.showRadar && metersB != 0){
                Guard(metersB, 1);
            }
            DrawRadarImage();
        }
        glPopMatrix();
        HandleBogeyCount(bogey_count);
        // Guard Zone image and heading line for radar only
        if (settings.showRadar) {
            double rotation = -settings.heading_correction + vp->skew * settings.skew_factor;
            if (!blackout[settings.selectRadarB]) rotation += br_hdt;
                glRotatef(rotation, 0, 0, 1); //  Undo heading correction, and add heading to get relative zones
                if (blackout[settings.selectRadarB]) {    // draw heading line
                glColor4ub(200, 0, 0, 50);
                glLineWidth(1);
                glBegin(GL_LINES);
                glVertex2d(0, 0);
                glVertex2d(br_range_meters[settings.selectRadarB] * v_scale_ppm, 0);
                glEnd();
            } 
                if (guardZones[settings.selectRadarB][0].type != GZ_OFF || guardZones[settings.selectRadarB][1].type != GZ_OFF) {
                    RenderGuardZone(radar_center, v_scale_ppm, vp, settings.selectRadarB);
            }
        }
    }
    glPopMatrix();
    glPopAttrib();
}        // end of RenderRadarOverlay


void br24radar_pi::DrawRadarImage()
{
    GLubyte alpha = 255 * (MAX_OVERLAY_TRANSPARENCY - settings.overlay_transparency) / MAX_OVERLAY_TRANSPARENCY;
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    time_t now = time(0);
    int step = 6 * sizeof(GLfloat);
    for (int i = 0; i < 2048; i++){
        if (now - vertices_time_stamp[i] > settings.max_age){
            continue;            // outdated line, do not display
        }
        glVertexPointer(2, GL_FLOAT, step, vertices[i]);
        glColorPointer(4, GL_FLOAT, step, vertices[i] + 2);
        int number_of_points = vertices_index[i] / 6;
        glDrawArrays(GL_TRIANGLES, 0, number_of_points);
    }
    glDisableClientState(GL_VERTEX_ARRAY);  // disable vertex arrays
    glDisableClientState(GL_COLOR_ARRAY);
}        // end of DrawRadarImage



void br24radar_pi::PrepareRadarImage(int angle)
{
    //    wxLongLong now = wxGetLocalTimeMillis();
    UINT32 drawn_spokes = 0;
    UINT32 drawn_blobs = 0;
    //    UINT32 skipped = 0;
    //    wxLongLong max_age = 0; // Age in millis

    GLubyte alpha = 255 * (MAX_OVERLAY_TRANSPARENCY - settings.overlay_transparency) / MAX_OVERLAY_TRANSPARENCY;

    vertices_index[angle] = 0;
    colors_index[angle] = 0;
    vertices_time_stamp[angle] = time(0);
    scan_line * scan = &m_scan_line[settings.selectRadarB][angle];

    int r_begin = 0, r_end = 0;
    enum colors { BLOB_NONE, BLOB_BLUE, BLOB_GREEN, BLOB_RED };
    colors actual_color = BLOB_NONE, previous_color = BLOB_NONE;
    drawn_spokes++;
    scan->data[RETURNS_PER_LINE] = 0;  // make sure this element is initialized (just outside the range)

    for (int radius = 0; radius <= RETURNS_PER_LINE; ++radius) {   // loop 1 more time as only the previous one will be displayed
        GLubyte strength = scan->data[radius];
        GLubyte hist = scan->history[radius];
        hist = hist & 7;  // check only last 3 bits

        if (((settings.multi_sweep_filter[settings.selectRadarB][2] == 1) && (!(hist == 3 || hist >= 5)) && radius != RETURNS_PER_LINE - 1)) {
            // corresponds to the patterns 011, 101, 110, 111
            // blob does not pass filter conditions
            actual_color = BLOB_NONE;
        }
        else   {     // blob passed filter or filter off
            switch (settings.display_option) {
                //  first find out the actual color
            case 0:
                actual_color = BLOB_NONE;
                if (strength > displaysetting0_threshold_red) actual_color = BLOB_RED;
                break;

            case 1:
                actual_color = BLOB_NONE;
                if (strength > displaysetting1_threshold_blue) actual_color = BLOB_BLUE;
                if (strength > 100) actual_color = BLOB_GREEN;
                if (strength > 200) actual_color = BLOB_RED;
                break;

            case 2:
                actual_color = BLOB_NONE;
                if (strength > displaysetting2_threshold_blue) actual_color = BLOB_BLUE;
                if (strength > 100) actual_color = BLOB_GREEN;
                if (strength > 250) {
                    actual_color = BLOB_RED;
                }
                break;
            }
        }

        if (actual_color == BLOB_NONE && previous_color == BLOB_NONE) {
            // nothing to do, next radius
            continue;
        }

        if (actual_color == previous_color) {
            // continue with same color, just register it
            r_end++;
        }
        else if (previous_color == BLOB_NONE && actual_color != BLOB_NONE) {
            // blob starts, no display, just register
            r_begin = radius;
            r_end = r_begin + 1;
            previous_color = actual_color;            // new color
        }
        else if (previous_color != BLOB_NONE && (previous_color != actual_color)) {
            // display time, first get the color in the glue byte
            GLubyte red = 0, green = 0, blue = 0;
            switch (previous_color) {
            case BLOB_RED:
                red = 255;
                break;
            case BLOB_GREEN:
                green = 255;
                break;
            case BLOB_BLUE:
                blue = 255;
                break;
            case BLOB_NONE:
                break;
            }

            draw_blob_gl_i(angle, r_begin, r_end, red, green, blue, alpha);
            drawn_blobs++;
            previous_color = actual_color;
            if (actual_color != BLOB_NONE) {            // change of color, start new blob
                r_begin = (double)radius;
                r_end = r_begin + 1;
            }
            else {            // actual_color == BLOB_NONE, blank pixel, next radius
                continue;
            }
        }
    }   // end of loop over radius
}        // end of PrepareRadarImage



void br24radar_pi::Guard(int max_range, int AB)
    // scan image for bogeys 
    {
    int begin_arc, end_arc = 0;
    for (size_t z = 0; z < GUARD_ZONES; z++) {  
        if (guardZones[AB][z].type == GZ_OFF){   // skip if guardzone is off
            continue;
        }
        switch (guardZones[AB][z].type) {
        case GZ_CIRCLE:
            begin_arc = 0;
            end_arc = LINES_PER_ROTATION;
            break;
        case GZ_ARC:
            begin_arc = guardZones[AB][z].start_bearing;
            end_arc = guardZones[AB][z].end_bearing;
            if (!blackout[AB]) {
                begin_arc += br_hdt;   // arc still in degrees!
                end_arc += br_hdt;
            }
            begin_arc = SCALE_DEGREES_TO_RAW2048 (begin_arc);      // br_hdt added to provide guard zone relative to heading
            end_arc = SCALE_DEGREES_TO_RAW2048(end_arc);    // now arc in lines
            
            begin_arc = MOD_ROTATION2048(begin_arc);
            end_arc = MOD_ROTATION2048(end_arc);
            break;
        default:
       //     wxLogMessage(wxT("BR24radar_pi: GuardZone %d: Off"), z + 1);
            begin_arc = 0;
            end_arc = 0;
            break;
            }
        if (begin_arc > end_arc) end_arc += LINES_PER_ROTATION;  // now end_arc may be larger than LINES_PER_ROTATION!

        for (int angle = begin_arc ; angle < end_arc ; angle++) {
            unsigned int angle1 = MOD_ROTATION2048 (angle);
            scan_line *scan = &m_scan_line[AB][angle1];
            if (!scan) return;   // No or old data
            for (int radius = 0; radius <= RETURNS_PER_LINE - 2; ++radius) { 
                // - 2 added, -1 contains the range circle, should not raise alarm
                //           if (guardZoneAngles[z][angle]) {
                int inner_range = guardZones[AB][z].inner_range; // now in meters
                int outer_range = guardZones[AB][z].outer_range; // now in meters
                int bogey_range = radius * max_range / RETURNS_PER_LINE;
                if (bogey_range > inner_range && bogey_range < outer_range) {   // within range, now check requirement for alarm
                    if ((settings.multi_sweep_filter[AB][z]) != 0) {  // multi sweep filter on for this z
                        GLubyte hist = scan->history[radius] & 7; // check only last 3 bits
                        if (!(hist == 3 || hist >= 5)) {  // corresponds to the patterns 011, 101, 110, 111
                            continue;                      // multi sweep filter on, no valid bogeys
                            }                   // so go to next radius
                        } 
                    else {   // multi sweep filter off
                        GLubyte strength = scan->data[radius];
                        if (strength <= displaysetting_threshold[settings.display_option]) continue;
                        }
                    int index = z + 2 * AB;
                    bogey_count[index]++;
                    }   // end "if (bogey_range > in ......

                }           // end of loop over radius
            }               // end of loop over angle
        }                   // end of loop over z
//wxLogMessage(wxT("BR24radar: Guard return, AB= %d bogeycount %d %d %d %d"), AB, bogey_count[0], bogey_count[1], bogey_count[2], bogey_count[3]);
    }
    


void br24radar_pi::draw_histogram_column(int x, int y)  // x=0->255 => 0->1020, y=0->100 =>0->400
{
    int xa = 5 * x;
    int xb = xa + 5;
    y = 4 * y;

    glBegin(GL_TRIANGLES);        // draw blob in two triangles
    glVertex2i(xa, 0);
    glVertex2i(xb, 0);
    glVertex2i(xa, y);

    glVertex2i(xb, 0);
    glVertex2i(xb, y);
    glVertex2i(xa, y);

    glEnd();

}


//****************************************************************************
void br24radar_pi::RenderGuardZone(wxPoint radar_center, double v_scale_ppm, PlugIn_ViewPort *vp, int AB)
{
    glPushAttrib(GL_COLOR_BUFFER_BIT | GL_LINE_BIT | GL_HINT_BIT);      //Save state
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    int start_bearing = 0, end_bearing = 0;
    GLubyte red = 0, green = 200, blue = 0, alpha = 50;

    for (size_t z = 0; z < GUARD_ZONES; z++) {

        if (guardZones[AB][z].type != GZ_OFF) {
            if (guardZones[AB][z].type == GZ_CIRCLE) {
                start_bearing = 0;
                end_bearing = 359;
            } else {
                start_bearing = guardZones[AB][z].start_bearing;
                end_bearing = guardZones[AB][z].end_bearing;
            }
            switch (settings.guard_zone_render_style) {
                case 1:
                    glColor4ub((GLubyte)255, (GLubyte)0, (GLubyte)0, (GLubyte)255);
                    DrawOutlineArc(guardZones[AB][z].outer_range * v_scale_ppm, guardZones[AB][z].inner_range * v_scale_ppm, start_bearing, end_bearing, true);
                    break;
                case 2:
                    glColor4ub(red, green, blue, alpha);
                    DrawOutlineArc(guardZones[AB][z].outer_range * v_scale_ppm, guardZones[AB][z].inner_range * v_scale_ppm, start_bearing, end_bearing, false);
                    // fall thru
                default:
                    glColor4ub(red, green, blue, alpha);
                    DrawFilledArc(guardZones[AB][z].outer_range * v_scale_ppm, guardZones[AB][z].inner_range * v_scale_ppm, start_bearing, end_bearing);
            }
        }

        red = 0; green = 0; blue = 200;
    }

    glPopAttrib();
}

void br24radar_pi::HandleBogeyCount(int *bogey_count)
{      // handles bogeys for both A and B in one go
    bool bogeysFound = false;
    for (int z = 0; z < 2 * GUARD_ZONES; z++) {
        if (bogey_count[z] > settings.guard_zone_threshold) {
            bogeysFound = true;
            break;
        }
    }
//    wxLogMessage(wxT("BR24radar_pi: handle bogeycount y=%d bogeysFound %d"),  y,bogeysFound);

    if (bogeysFound) {
        // We have bogeys and there is no objection to showing the dialog
        if (settings.timed_idle != 0) m_pControlDialog->SetTimedIdleIndex(0); //Disable Timed Idle if set

        if (!m_pGuardZoneBogey && settings.showRadar) {
            // If this is the first time we have a bogey create & show the dialog immediately
            m_pGuardZoneBogey = new GuardZoneBogey;
            m_pGuardZoneBogey->Create(m_parent_window, this);
            m_pGuardZoneBogey->Show();
            m_pGuardZoneBogey->SetPosition(wxPoint(m_GuardZoneBogey_x, m_GuardZoneBogey_y));
        }
        else if (!br_guard_bogey_confirmed && (settings.showRadar)) {
            m_pGuardZoneBogey->Show();
        }
        time_t now = time(0);
        int delta_t = now - br_alarm_sound_last;
        if (!br_guard_bogey_confirmed && delta_t >= ALARM_TIMEOUT && bogeysFound) {
            // If the last time is 10 seconds ago we ping a sound, unless the user confirmed
            br_alarm_sound_last = now;

            if (!settings.alert_audio_file.IsEmpty()) {
                PlugInPlaySound(settings.alert_audio_file);
            }
            else {
                wxBell();
            }  // end of ping
            if (m_pGuardZoneBogey && settings.showRadar) {
                m_pGuardZoneBogey->Show();
            }
            delta_t = ALARM_TIMEOUT;
        }
        if (m_pGuardZoneBogey) {
            m_pGuardZoneBogey->SetBogeyCount(bogey_count, br_guard_bogey_confirmed ? -1 : ALARM_TIMEOUT - delta_t);
        }
    }

    if (!bogeysFound && m_pGuardZoneBogey) {
        m_pGuardZoneBogey->SetBogeyCount(bogey_count, -1);   // with -1 "next alarm in... "will not be displayed
        br_guard_bogey_confirmed = false; // Reset for next time we see bogeys
        // keep showing the bogey dialogue with 0 bogeys
    }
    
}



//****************************************************************************

bool br24radar_pi::LoadConfig(void)
{

    wxFileConfig *pConf = m_pconfig;

    if (pConf) {

        wxString sll;
        double lat, lon;
        pConf->SetPath(wxT("/Settings/GlobalState"));
        if (pConf->Read(wxT("OwnShipLatLon"), &sll)) {
            sscanf(sll.mb_str(wxConvUTF8), "%lf,%lf", &lat, &lon);

            //    Sanity check the lat/lon...both have to be reasonable.
            if (fabs(lon) < 360.) {
                while (lon < -180.)
                    lon += 360.;
                while (lon > 180.)
                    lon -= 360.;
                gLon = lon;
            }
            if (fabs(lat) < 90.0) gLat = lat;
        }
            wxLogMessage(wxT("BR24radar_pi:  latlon read %g %g"), gLat, gLon);
            
        pConf->SetPath(wxT("/Plugins/BR24Radar"));
        pConf->Read(wxT("DisplayOption"), &settings.display_option, 0);
        pConf->Read(wxT("RangeUnits" ), &settings.range_units, 0 ); //0 = "Nautical miles"), 1 = "Kilometers"
        if (settings.range_units >= 2) {
            settings.range_units = 1;
        }
        settings.range_unit_meters = (settings.range_units == 1) ? 1000 : 1852;
        pConf->Read(wxT("DisplayMode"),  (int *) &settings.display_mode[0], 0);
        pConf->Read(wxT("DisplayModeB"), (int *)&settings.display_mode[1], 0);
    //    pConf->Read(wxT("EmulatorOn"), (int *)&settings.emulator_on, 0);
        pConf->Read(wxT("VerboseLog"),  &settings.verbose, 0);
        pConf->Read(wxT("Transparency"),  &settings.overlay_transparency, DEFAULT_OVERLAY_TRANSPARENCY);
        pConf->Read(wxT("RangeCalibration"),  &settings.range_calibration, 1.0);
        pConf->Read(wxT("HeadingCorrection"),  &settings.heading_correction, 0);
        pConf->Read(wxT("ScanMaxAge"), &settings.max_age, 6);   // default 6
        if (settings.max_age < MIN_AGE) {
            settings.max_age = MIN_AGE;
        } else if (settings.max_age > MAX_AGE) {
            settings.max_age = MAX_AGE;
        }
        pConf->Read(wxT("RunTimeOnIdle"), &settings.idle_run_time, 2);
        pConf->Read(wxT("DrawAlgorithm"), &settings.draw_algorithm, 1);
        pConf->Read(wxT("GuardZonesThreshold"), &settings.guard_zone_threshold, 5L);
        pConf->Read(wxT("GuardZonesRenderStyle"), &settings.guard_zone_render_style, 0);
        pConf->Read(wxT("Refreshrate"), &settings.refreshrate, 1);
        if (settings.refreshrate < 1) {
            settings.refreshrate = 1; // not allowed
        }
        if (settings.refreshrate > 5) {
            settings.refreshrate = 5; // not allowed
        }
        br_refresh_rate = REFRESHMAPPING[settings.refreshrate - 1];

        pConf->Read(wxT("PassHeadingToOCPN"), &settings.passHeadingToOCPN, 0);
        pConf->Read(wxT("selectRadarB"), &settings.selectRadarB, 0);
    /*    if (settings.emulator_on) {
            settings.selectRadarB = 0;
        }*/
        pConf->Read(wxT("ControlsDialogSizeX"), &m_BR24Controls_dialog_sx, 300L);
        pConf->Read(wxT("ControlsDialogSizeY"), &m_BR24Controls_dialog_sy, 540L);
        pConf->Read(wxT("ControlsDialogPosX"), &m_BR24Controls_dialog_x, 20L);
        pConf->Read(wxT("ControlsDialogPosY"), &m_BR24Controls_dialog_y, 170L);

        pConf->Read(wxT("MessageBoxSizeX"), &m_BR24Message_box_sx, 300L);
        pConf->Read(wxT("MessageBoxSizeY"), &m_BR24Message_box_sy, 540L);
        pConf->Read(wxT("MessageBoxPosX"), &m_BR24Message_box_x, 10L);  
        pConf->Read(wxT("MessageBoxPosY"), &m_BR24Message_box_y, 150L);

        pConf->Read(wxT("GuardZonePosX"), &m_GuardZoneBogey_x, 20L);
        pConf->Read(wxT("GuardZonePosY"), &m_GuardZoneBogey_y, 170L);

        pConf->Read(wxT("Zone1StBrng"), &guardZones[0][0].start_bearing, 0.0);
        pConf->Read(wxT("Zone1EndBrng"), &guardZones[0][0].end_bearing, 0.0);
        pConf->Read(wxT("Zone1OuterRng"), &guardZones[0][0].outer_range, 0);
        pConf->Read(wxT("Zone1InnerRng"), &guardZones[0][0].inner_range, 0);
        pConf->Read(wxT("Zone1ArcCirc"), &guardZones[0][0].type, 0);

        pConf->Read(wxT("Zone2StBrng"), &guardZones[0][1].start_bearing, 0.0);
        pConf->Read(wxT("Zone2EndBrng"), &guardZones[0][1].end_bearing, 0.0);
        pConf->Read(wxT("Zone2OuterRng"), &guardZones[0][1].outer_range, 0);
        pConf->Read(wxT("Zone2InnerRng"), &guardZones[0][1].inner_range, 0);
        pConf->Read(wxT("Zone2ArcCirc"), &guardZones[0][1].type, 0);

        pConf->Read(wxT("Zone1StBrngB"), &guardZones[1][0].start_bearing, 0.0);
        pConf->Read(wxT("Zone1EndBrngB"), &guardZones[1][0].end_bearing, 0.0);
        pConf->Read(wxT("Zone1OuterRngB"), &guardZones[1][0].outer_range, 0);
        pConf->Read(wxT("Zone1InnerRngB"), &guardZones[1][0].inner_range, 0);
        pConf->Read(wxT("Zone1ArcCircB"), &guardZones[1][0].type, 0);

        pConf->Read(wxT("Zone2StBrngB"), &guardZones[1][1].start_bearing, 0.0);
        pConf->Read(wxT("Zone2EndBrngB"), &guardZones[1][1].end_bearing, 0.0);
        pConf->Read(wxT("Zone2OuterRngB"), &guardZones[1][1].outer_range, 0);
        pConf->Read(wxT("Zone2InnerRngB"), &guardZones[1][1].inner_range, 0);
        pConf->Read(wxT("Zone2ArcCircB"), &guardZones[1][1].type, 0);

        pConf->Read(wxT("RadarAlertAudioFile"), &settings.alert_audio_file);
        pConf->Read(wxT("EnableDualRadar"), &settings.enable_dual_radar, 0);

        pConf->Read(wxT("SkewFactor"), &settings.skew_factor, 1);

        SaveConfig();
        return true;
    }
    return false;
}

bool br24radar_pi::SaveConfig(void)
{

    wxFileConfig *pConf = m_pconfig;

    if (pConf) {
        pConf->SetPath(wxT("/Plugins/BR24Radar"));
        pConf->Write(wxT("DisplayOption"), settings.display_option);
        pConf->Write(wxT("RangeUnits" ), settings.range_units);
        pConf->Write(wxT("DisplayMode"), (int)settings.display_mode[0]);
//        pConf->Write(wxT("EmulatorOn"), (int)settings.emulator_on);
        pConf->Write(wxT("DisplayModeB"), (int)settings.display_mode[1]);
        pConf->Write(wxT("VerboseLog"), settings.verbose);
        pConf->Write(wxT("Transparency"), settings.overlay_transparency);
        pConf->Write(wxT("RangeCalibration"),  settings.range_calibration);
        pConf->Write(wxT("HeadingCorrection"),  settings.heading_correction);
        pConf->Write(wxT("GuardZonesThreshold"), settings.guard_zone_threshold);
        pConf->Write(wxT("GuardZonesRenderStyle"), settings.guard_zone_render_style);
        pConf->Write(wxT("ScanMaxAge"), settings.max_age);
        pConf->Write(wxT("RunTimeOnIdle"), settings.idle_run_time);
        pConf->Write(wxT("DrawAlgorithm"), settings.draw_algorithm);
        pConf->Write(wxT("Refreshrate"), settings.refreshrate);
        pConf->Write(wxT("PassHeadingToOCPN"), settings.passHeadingToOCPN);
        pConf->Write(wxT("selectRadarB"), settings.selectRadarB);
        pConf->Write(wxT("ShowRadar"), settings.showRadar);
        pConf->Write(wxT("RadarAlertAudioFile"), settings.alert_audio_file);
        pConf->Write(wxT("EnableDualRadar"), settings.enable_dual_radar);
        pConf->Write(wxT("ControlsDialogSizeX"),  m_BR24Controls_dialog_sx);
        pConf->Write(wxT("ControlsDialogSizeY"),  m_BR24Controls_dialog_sy);
        pConf->Write(wxT("ControlsDialogPosX"),   m_BR24Controls_dialog_x);
        pConf->Write(wxT("ControlsDialogPosY"),   m_BR24Controls_dialog_y);

        pConf->Write(wxT("MessageBoxSizeX"), m_BR24Message_box_sx);
        pConf->Write(wxT("MessageBoxSizeY"), m_BR24Message_box_sy);
        pConf->Write(wxT("MessageBoxPosX"), m_BR24Message_box_x);
        pConf->Write(wxT("MessageBoxPosY"), m_BR24Message_box_y);

        pConf->Write(wxT("GuardZonePosX"),   m_GuardZoneBogey_x);
        pConf->Write(wxT("GuardZonePosY"),   m_GuardZoneBogey_y);

        pConf->Write(wxT("Zone1StBrng"), guardZones[0][0].start_bearing);
        pConf->Write(wxT("Zone1EndBrng"), guardZones[0][0].end_bearing);
        pConf->Write(wxT("Zone1OuterRng"), guardZones[0][0].outer_range);
        pConf->Write(wxT("Zone1InnerRng"), guardZones[0][0].inner_range);
        pConf->Write(wxT("Zone1ArcCirc"), guardZones[0][0].type);

        pConf->Write(wxT("Zone2StBrng"), guardZones[0][1].start_bearing);
        pConf->Write(wxT("Zone2EndBrng"), guardZones[0][1].end_bearing);
        pConf->Write(wxT("Zone2OuterRng"), guardZones[0][1].outer_range);
        pConf->Write(wxT("Zone2InnerRng"), guardZones[0][1].inner_range);
        pConf->Write(wxT("Zone2ArcCirc"), guardZones[0][1].type);

        pConf->Write(wxT("Zone1StBrngB"), guardZones[1][0].start_bearing);
        pConf->Write(wxT("Zone1EndBrngB"), guardZones[1][0].end_bearing);
        pConf->Write(wxT("Zone1OuterRngB"), guardZones[1][0].outer_range);
        pConf->Write(wxT("Zone1InnerRngB"), guardZones[1][0].inner_range);
        pConf->Write(wxT("Zone1ArcCircB"), guardZones[1][0].type);

        pConf->Write(wxT("Zone2StBrngB"), guardZones[1][1].start_bearing);
        pConf->Write(wxT("Zone2EndBrngB"), guardZones[1][1].end_bearing);
        pConf->Write(wxT("Zone2OuterRngB"), guardZones[1][1].outer_range);
        pConf->Write(wxT("Zone2InnerRngB"), guardZones[1][1].inner_range);
        pConf->Write(wxT("Zone2ArcCircB"), guardZones[1][1].type);

        pConf->Write(wxT("SkewFactor"), settings.skew_factor);

        pConf->Flush();
        return true;
    }

    return false;
}


// Positional Data passed from NMEA to plugin
void br24radar_pi::SetPositionFix(PlugIn_Position_Fix &pfix)
{
}

void br24radar_pi::SetPositionFixEx(PlugIn_Position_Fix_Ex &pfix)
{
    time_t now = time(0);
    wxString info;

    // PushNMEABuffer (_("$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,,230394,003.1,W"));  // only for test, position without heading
    // PushNMEABuffer (_("$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A")); //with heading for test
    if (br_var_source <= VARIATION_SOURCE_FIX && !wxIsNaN(pfix.Var) && (fabs(pfix.Var) > 0.0 || br_var == 0.0)) {
        if (br_var_source < VARIATION_SOURCE_FIX || fabs(pfix.Var - br_var) > 0.05) {
            wxLogMessage(wxT("BR24radar_pi: Position fix provides new magnetic variation %f"), pfix.Var);
            if (m_pMessageBox) {
                if (m_pMessageBox->IsShown()) {
                    info = _("GPS");
                    info << wxT(" ") << br_var;
                    m_pMessageBox->SetVariationInfo(info);
                }
            }
        }
        br_var = pfix.Var;
        br_var_source = VARIATION_SOURCE_FIX;
        br_var_watchdog = now;
    }

    if (settings.verbose >= 2) {
        wxLogMessage(wxT("BR24radar_pi: SetPositionFixEx var=%f heading_on_radar=%d br_var_wd=%d settings.showRadar=%d")
                    , pfix.Var
                    , br_heading_on_radar
                    , TIMER_NOT_ELAPSED(br_var_watchdog)
                    , settings.showRadar
                    );
    }
    if (br_heading_on_radar && TIMER_NOT_ELAPSED(br_var_watchdog) && settings.showRadar) {
        if (m_heading_source != HEADING_RADAR) {
           if(settings.verbose) wxLogMessage(wxT("BR24radar_pi: Heading source is now Radar %f \n"), br_hdt);
            m_heading_source = HEADING_RADAR;
        }
        if (m_pMessageBox) {
            if (m_pMessageBox->IsShown()) {
                info = _("radar");
                info << wxT(" ") << br_hdt;
                m_pMessageBox->SetHeadingInfo(info);
            }
        }
        br_hdt_watchdog = now;
    }
    else if (!wxIsNaN(pfix.Hdm) && TIMER_NOT_ELAPSED(br_var_watchdog)) {
        br_hdt = pfix.Hdm + br_var;
        if (m_heading_source != HEADING_HDM) {
            wxLogMessage(wxT("BR24radar_pi: Heading source is now HDM %f"), br_hdt);
            m_heading_source = HEADING_HDM;
        }
        if (m_pMessageBox) {
            if (m_pMessageBox->IsShown()) {
                info = _("HDM");
                info << wxT(" ") << br_hdt;
                m_pMessageBox->SetHeadingInfo(info);
            }
        }
        br_hdt_watchdog = now;
    }
    else if (!wxIsNaN(pfix.Hdt)) {
        br_hdt = pfix.Hdt;
        if (m_heading_source != HEADING_HDT) {
            wxLogMessage(wxT("BR24radar_pi: Heading source is now HDT"));
            m_heading_source = HEADING_HDT;
        }
        if (m_pMessageBox) {
            if (m_pMessageBox->IsShown()) {
                info = _("HDT");
                info << wxT(" ") << br_hdt;
                m_pMessageBox->SetHeadingInfo(info);
            }
        }
        br_hdt_watchdog = now;
    }
    else if (!wxIsNaN(pfix.Cog)) {
        br_hdt = pfix.Cog;
        if (m_heading_source != HEADING_COG) {
            wxLogMessage(wxT("BR24radar_pi: Heading source is now COG"));
            m_heading_source = HEADING_COG;
        }
        if (m_pMessageBox) {
            if (m_pMessageBox->IsShown()) {
                info = _("COG");
                info << wxT(" ") << br_hdt;
                m_pMessageBox->SetHeadingInfo(info);
            }
        }
        br_hdt_watchdog = now;
    }

    if (pfix.FixTime && TIMER_NOT_ELAPSED(pfix.FixTime)) {
        br_ownship_lat = pfix.Lat;
        br_ownship_lon = pfix.Lon;
        if (!br_bpos_set) {
            wxLogMessage(wxT("BR24radar_pi: GPS position is now known"));
        }
        br_bpos_set = true;
        br_bpos_watchdog = now;
    }
}    // end of br24radar_pi::SetPositionFixEx(PlugIn_Position_Fix_Ex &pfix)

void br24radar_pi::SetPluginMessage(wxString &message_id, wxString &message_body)
{
    static const wxString WMM_VARIATION_BOAT = wxString(_T("WMM_VARIATION_BOAT"));
    if (message_id.Cmp(WMM_VARIATION_BOAT) == 0) {
        wxJSONReader reader;
        wxJSONValue message;
        if (!reader.Parse(message_body, &message)) {
            wxJSONValue defaultValue(360);
            double variation = message.Get(_T("Decl"), defaultValue).AsDouble();

            if (variation != 360.0) {
                if (br_var_source != VARIATION_SOURCE_WMM) {
                    wxLogMessage(wxT("BR24radar_pi: WMM plugin provides new magnetic variation %f"), variation);
                }
                br_var = variation;
                br_var_source = VARIATION_SOURCE_WMM;
                br_var_watchdog = time(0);
                if (m_pMessageBox) {
                    if (m_pMessageBox->IsShown()) {
                        wxString info = _("WMM");
                        info << wxT(" ") << br_var;
                        m_pMessageBox->SetVariationInfo(info);
                    }
                }
            }
        }
    }
}

//**************** Cursor position events **********************
void br24radar_pi::SetCursorLatLon(double lat, double lon)
{
    br_cur_lat = lat;
    br_cur_lon = lon;
}

//************************************************************************
// Plugin Command Data Streams
//************************************************************************
bool br24radar_pi::TransmitCmd(UINT8 * msg, int size)
{
    struct sockaddr_in adr;
    memset(&adr, 0, sizeof(adr));
    adr.sin_family = AF_INET;
    if (settings.selectRadarB == 1) {   //  select B radar
    adr.sin_addr.s_addr=htonl((236 << 24) | (6 << 16) | (7 << 8) | 14); // 236.6.7.14
    adr.sin_port=htons(6658);
    }
    else {    // select A radar
        adr.sin_addr.s_addr=htonl((236 << 24) | (6 << 16) | (7 << 8) | 10); // 236.6.7.10
    adr.sin_port=htons(6680);
    }
    if (m_radar_socket == INVALID_SOCKET || sendto(m_radar_socket, (char *) msg, size, 0, (struct sockaddr *) &adr, sizeof(adr)) < size) {
        wxLogError(wxT("BR24radar_pi: Unable to transmit command to radar"));
        return(false);
    } else  {
        return(true);
    }
};

bool br24radar_pi::TransmitCmd(int AB, UINT8 * msg, int size)
{                                 //    overcharged version allows selection of radar
    struct sockaddr_in adr;
    memset(&adr, 0, sizeof(adr));
    adr.sin_family = AF_INET;
    if (AB == 1) {   //  select B radar
        adr.sin_addr.s_addr = htonl((236 << 24) | (6 << 16) | (7 << 8) | 14); // 236.6.7.14
        adr.sin_port = htons(6658);
    }
    else {    // select A radar
        adr.sin_addr.s_addr = htonl((236 << 24) | (6 << 16) | (7 << 8) | 10); // 236.6.7.10
        adr.sin_port = htons(6680);
    }
    if (m_radar_socket == INVALID_SOCKET || sendto(m_radar_socket, (char *)msg, size, 0, (struct sockaddr *) &adr, sizeof(adr)) < size) {
        wxLogError(wxT("BR24radar_pi: Unable to transmit command to radar"));
        return(false);
    }
    else  {
        return(true);
    }
};

void br24radar_pi::RadarTxOff(void)
{          
    if(settings.enable_dual_radar == 0){   // switch active radar off
        UINT8 pck[3] = {0x00, 0xc1, 0x01};
        TransmitCmd(settings.selectRadarB, pck, sizeof(pck));
        pck[0] = 0x01;
        pck[2] = 0x00;
        TransmitCmd(settings.selectRadarB, pck, sizeof(pck));
    }
    else{              // switch both A and B off
        UINT8 pck[3] = {0x00, 0xc1, 0x01};
        TransmitCmd(0, pck, sizeof(pck));
        pck[0] = 0x01;
        pck[2] = 0x00;
        TransmitCmd(0, pck, sizeof(pck));

        pck[0] = 0x00;
        pck[2] = 0x01;
        TransmitCmd(1, pck, sizeof(pck));
        pck[0] = 0x01;
        pck[2] = 0x00;
        TransmitCmd(1, pck, sizeof(pck));
    }
}

void br24radar_pi::RadarTxOn(void)
{                                          // turn A on
    if (settings.enable_dual_radar == 0){ 
    UINT8 pck[3] = { 0x00, 0xc1, 0x01 };               // ON
    TransmitCmd(settings.selectRadarB, pck, sizeof(pck));
    if (settings.verbose) wxLogMessage(wxT("BR24radar_pi: Turn radar %d on (send TRANSMIT request)"), settings.selectRadarB);
    pck[0] = 0x01;
    TransmitCmd(settings.selectRadarB, pck, sizeof(pck));
    }
    else{                               // turn A and B both on 
    UINT8 pck[3] = { 0x00, 0xc1, 0x01 };               // ON
    TransmitCmd(0, pck, sizeof(pck));
    if (settings.verbose)wxLogMessage(wxT("BR24radar_pi: Turn radar %d on (send TRANSMIT request)"), settings.selectRadarB);
    pck[0] = 0x01;
    TransmitCmd(0, pck, sizeof(pck));
 
    UINT8 pckb[3] = { 0x00, 0xc1, 0x01 };               // ON
    TransmitCmd(1, pckb, sizeof(pck));
    if (settings.verbose)wxLogMessage(wxT("BR24radar_pi: Turn radar %d on (send TRANSMIT request)"), settings.selectRadarB);
    pckb[0] = 0x01;
    TransmitCmd(1, pckb, sizeof(pck));
    }
}

bool br24radar_pi::RadarStayAlive(void)
{
    UINT8 pck[] = {0xA0, 0xc1};
    TransmitCmd(settings.selectRadarB, pck, sizeof(pck));
    UINT8 pck2[] = { 0x03, 0xc2 };
    TransmitCmd(settings.selectRadarB, pck2, sizeof(pck2));
    UINT8 pck3[] = { 0x04, 0xc2 };
    TransmitCmd(settings.selectRadarB, pck3, sizeof(pck3));
    UINT8 pck4[] = { 0x05, 0xc2 };
    bool succes = TransmitCmd(settings.selectRadarB, pck4, sizeof(pck4));  

    if (settings.enable_dual_radar){    // also send on the other channel
        UINT8 pck[] = {0xA0, 0xc1};
        TransmitCmd(!settings.selectRadarB, pck, sizeof(pck));
        UINT8 pck2[] = { 0x03, 0xc2 };
        TransmitCmd(!settings.selectRadarB, pck2, sizeof(pck2));
        UINT8 pck3[] = { 0x04, 0xc2 };
        TransmitCmd(!settings.selectRadarB, pck3, sizeof(pck3));
        UINT8 pck4[] = { 0x05, 0xc2 };
        TransmitCmd(!settings.selectRadarB, pck4, sizeof(pck4));  
    }
    return(succes);
}



void br24radar_pi::SetRangeMeters(long meters)
{
   if (br_radar_seen) {
        if (meters >= 50 && meters <= 72704) {
            long decimeters = meters * 10L;
            UINT8 pck[] =
                { 0x03
                , 0xc1
                , (UINT8) ((decimeters >>  0) & 0XFFL)
                , (UINT8) ((decimeters >>  8) & 0XFFL)
                , (UINT8) ((decimeters >> 16) & 0XFFL)
                , (UINT8) ((decimeters >> 24) & 0XFFL)
                };
            if (settings.verbose) {
                wxLogMessage(wxT("BR24radar_pi: SetRangeMeters: range %ld meters\n"), meters);
            }
            TransmitCmd(pck, sizeof(pck));
            br_commanded_range_meters = meters;
        }
    }
}

void radar_control_item::Update(int v)
{
    if (v != button){
        mod = true;
    //    value = v;   // not needed? may be for range later
        button = v;
        }
};

void br24radar_pi::SetControlValue(ControlType controlType, int value)
{                                                   // sends the command to the radar
    wxString msg;
    if (br_radar_seen || controlType == CT_TRANSPARENCY || controlType == CT_SCAN_AGE || CT_REFRESHRATE) {
        switch (controlType) {
            case CT_GAIN: {
      //          settings.gain = value;
                if (value < 0) {                // AUTO gain
                    UINT8 cmd[] = {
                        0x06,
                        0xc1,
                        0, 0, 0, 0, 0x01,
                        0, 0, 0, 0xad     // changed from a1 to ad right ????   no changed back
                    };
                    if (settings.verbose) {
                        wxLogMessage(wxT("BR24radar_pi: Gain: Auto in setcontrolvalue"));
                    }
                    TransmitCmd(cmd, sizeof(cmd));
                } else {                        // Manual Gain
                    int v = (value + 1) * 255 / 100;
                    if (v > 255) {
                        v = 255;
                    }
                    UINT8 cmd[] = {
                        0x06,
                        0xc1,
                        0, 0, 0, 0, 0, 0, 0, 0,
                        (UINT8) v
                    };
                    if (settings.verbose) {
                        wxLogMessage(wxT("BR24radar_pi: Gain: %d"), value);
                    }
                    TransmitCmd(cmd, sizeof(cmd));
                }
                break;
            }
            case CT_RAIN: {                       // Rain Clutter - Manual. Range is 0x01 to 0x50
                int v = (value + 1) * 255 / 100;
                if (v > 255) {
                    v = 255;
                }

                UINT8 cmd[] = {
                    0x06,
                    0xc1,
                    0x04,
                    0, 0, 0, 0, 0, 0, 0,
                    (UINT8) v
                };
                if (settings.verbose) {
                    wxLogMessage(wxT("BR24radar_pi: Rain: %d"), value);
                }
                TransmitCmd(cmd, sizeof(cmd));
                break;
            }
            case CT_SEA: {
  //              settings.sea_clutter_gain = value;
                if (value < 0) {                 // Sea Clutter - Auto
                    UINT8 cmd[11] = {
                        0x06,
                        0xc1,
                        0x02,
                        0, 0, 0, 0x01,
                        0, 0, 0, 0xd3
                    };
                    if (settings.verbose) {
                        wxLogMessage(wxT("BR24radar_pi: Sea: Auto"));
                    }
                    TransmitCmd(cmd, sizeof(cmd));
                } else {                       // Sea Clutter
                    int v = (value + 1) * 255 / 100 ;
                    if (v > 255) {
                        v = 255;
                    }
                    UINT8 cmd[] = {
                        0x06,
                        0xc1,
                        0x02,
                        0, 0, 0, 0, 0, 0, 0,
                        (UINT8) v
                    };
                    if (settings.verbose) {
                        wxLogMessage(wxT("BR24radar_pi: Sea: %d"), value);
                    }
                    TransmitCmd(cmd, sizeof(cmd));
                }
                break;
            }
            case CT_INTERFERENCE_REJECTION: {
    //            settings.interference_rejection = value;
                UINT8 cmd[] = {
                    0x08,
                    0xc1,
                    (UINT8) value
                };
                if (settings.verbose) {
                    wxLogMessage(wxT("BR24radar_pi: Rejection: %d"), value);
                }
                TransmitCmd(cmd, sizeof(cmd));
                break;
            }
            case CT_TARGET_SEPARATION: {
     //           settings.target_separation = value;
                UINT8 cmd[] = {
                    0x22,
                    0xc1,
                    (UINT8) value
                };
                if (settings.verbose) {
                    wxLogMessage(wxT("BR24radar_pi: Target separation: %d"), value);
                }
                TransmitCmd(cmd, sizeof(cmd));
                break;
            }
            case CT_NOISE_REJECTION: {
     //           settings.noise_rejection = value;
                UINT8 cmd[] = {
                    0x21,
                    0xc1,
                    (UINT8) value
                };
                if (settings.verbose) {
                    wxLogMessage(wxT("BR24radar_pi: Noise rejection: %d"), value);
                }
                TransmitCmd(cmd, sizeof(cmd));
                break;
            }
            case CT_TARGET_BOOST: {
                UINT8 cmd[] = {
                    0x0a,
                    0xc1,
                    (UINT8) value
                };
                if (settings.verbose) {
                    wxLogMessage(wxT("BR24radar_pi: Target boost: %d"), value);
                }
                TransmitCmd(cmd, sizeof(cmd));
                break;
            }
            case CT_SCAN_SPEED: {
                UINT8 cmd[] = {
                    0x0f,
                    0xc1,
                    (UINT8) value
                };
                if (settings.verbose) {
                    wxLogMessage(wxT("BR24radar_pi: Scan speed: %d"), value);
                }
                TransmitCmd(cmd, sizeof(cmd));
                break;
            }
            case CT_TRANSPARENCY: {
                settings.overlay_transparency = value;
                break;
            }
            case CT_SCAN_AGE: {
                settings.max_age = value;
                break;
            }
            case CT_TIMED_IDLE: {
                settings.timed_idle = value;
                break;
            }
            
            case CT_REFRESHRATE: {
                settings.refreshrate = value;
                break;
                                 }

            case CT_ANTENNA_HEIGHT: {   
                int v = value * 1000;
                int v1 = v / 256;
                int v2 = v - 256 * v1;
                UINT8 cmd[10] = { 0x30, 0xc1, 0x01, 0, 0, 0,
                    (UINT8)v2, (UINT8)v1, 0, 0 };
                if (settings.verbose) {
                    wxLogMessage(wxT("BR24radar_pi: Antenna height: %d"), v);
                }
                TransmitCmd(cmd, sizeof(cmd));
                break;
            }

            case CT_BEARING_ALIGNMENT: {   // to be consistent with the local bearing alignment of the pi
                                           // this bearing alignment works opposite to the one an a Lowrance display
                if (value < 0){
                    value += 360;
                }
                int v = value * 10;
                int v1 = v / 256;
                int v2 = v - 256 * v1;
                UINT8 cmd[4] = { 0x05, 0xc1,
                    (UINT8)v2, (UINT8)v1 };
                if (settings.verbose) {
                    wxLogMessage(wxT("BR24radar_pi: Bearing alignment: %d"), v);
                }
                TransmitCmd(cmd, sizeof(cmd));
                break;
            }

            case CT_SIDE_LOBE_SUPPRESSION: {
                if (value < 0) {
                    UINT8 cmd[] = {                 // SIDE_LOBE_SUPPRESSION auto
                        0x06, 0xc1, 0x05, 0, 0, 0, 0x01, 0, 0, 0, 0xc0 };
                    if (settings.verbose) {
                        wxLogMessage(wxT("BR24radar_pi: command Tx CT_SIDE_LOBE_SUPPRESSION Auto"));
                    }
                    TransmitCmd(cmd, sizeof(cmd));
                }
                else{
                    int v = (value + 1) * 255 / 100;
                    if (v > 255) {
                        v = 255;
                    }
                    UINT8 cmd[] = {
                        0x6, 0xc1, 0x05, 0, 0, 0, 0, 0, 0, 0,
                        (UINT8)v
                    };
                    if (settings.verbose) {
                        wxLogMessage(wxT("BR24radar_pi: command Tx CT_SIDE_LOBE_SUPPRESSION: %d"), value);
                    }
                    TransmitCmd(cmd, sizeof(cmd));  
                }
                break;
            }

            case CT_LOCAL_INTERFERENCE_REJECTION: {
                if (value < 0) value = 0;
                if (value > 3) value = 3;
                UINT8 cmd[] = {
                    0x0e,
                    0xc1,
                    (UINT8)value
                };
                if (settings.verbose) {
                    wxLogMessage(wxT("BR24radar_pi: Local interference rejection %d"), value);
                }
                TransmitCmd(cmd, sizeof(cmd));
                break;
            }

            
            default: {
                wxLogMessage(wxT("BR24radar_pi: Unhandled control setting for control %d"), controlType);
            }
        }
    }
}

//*****************************************************************************************************
void br24radar_pi::CacheSetToolbarToolBitmaps(int bm_id_normal, int bm_id_rollover)
{
    if ((bm_id_normal == m_sent_bm_id_normal) && (bm_id_rollover == m_sent_bm_id_rollover)) {
        return;    // no change needed
    }

    if ((bm_id_normal == -1) || (bm_id_rollover == -1)) {         // don't do anything, caller's responsibility
        m_sent_bm_id_normal = bm_id_normal;
        m_sent_bm_id_rollover = bm_id_rollover;
        return;
    }

    m_sent_bm_id_normal = bm_id_normal;
    m_sent_bm_id_rollover = bm_id_rollover;

    wxBitmap *pnormal = NULL;
    wxBitmap *prollover = NULL;

    switch (bm_id_normal) {
        case BM_ID_RED:
            pnormal = _img_radar_red;
            break;
        case BM_ID_RED_SLAVE:
            pnormal = _img_radar_red_slave;
            break;
        case BM_ID_GREEN:
            pnormal = _img_radar_green;
            break;
        case BM_ID_GREEN_SLAVE:
            pnormal = _img_radar_green_slave;
            break;
        case BM_ID_AMBER:
            pnormal = _img_radar_amber;
            break;
        case BM_ID_AMBER_SLAVE:
            pnormal = _img_radar_amber_slave;
            break;
        case BM_ID_BLANK:
            pnormal = _img_radar_blank;
            break;
        case BM_ID_BLANK_SLAVE:
            pnormal = _img_radar_blank_slave;
            break;
        default:
            break;
    }

    switch (bm_id_rollover) {
        case BM_ID_RED:
            prollover = _img_radar_red;
            break;
        case BM_ID_RED_SLAVE:
            prollover = _img_radar_red_slave;
            break;
        case BM_ID_GREEN:
            prollover = _img_radar_green;
            break;
        case BM_ID_GREEN_SLAVE:
            prollover = _img_radar_green_slave;
            break;
        case BM_ID_AMBER:
            prollover = _img_radar_amber;
            break;
        case BM_ID_AMBER_SLAVE:
            prollover = _img_radar_amber_slave;
            break;
        case BM_ID_BLANK:
            prollover = _img_radar_blank;
            break;
        case BM_ID_BLANK_SLAVE:
            prollover = _img_radar_blank_slave;
            break;
        default:
            break;
    }

    if ((pnormal) && (prollover)) {
        SetToolbarToolBitmaps(m_tool_id, pnormal, prollover);
    }
}


/*
   SetNMEASentence is used to speed up rotation and variation
   detection if SetPositionEx() is not called very often. This will
   be the case if you have a high speed heading sensor (for instance, 2 to 20 Hz)
   but only a 1 Hz GPS update.

   Note that the type of heading source is only updated in SetPositionEx().
*/

void br24radar_pi::SetNMEASentence( wxString &sentence )
{
    m_NMEA0183 << sentence;
    time_t now = time(0);

    if (m_NMEA0183.PreParse()) {
        if (m_NMEA0183.LastSentenceIDReceived == _T("HDG") && m_NMEA0183.Parse()) {
            if (settings.verbose >= 2) {
                wxLogMessage(wxT("BR24radar_pi: received HDG variation=%f var_source=%d br_var=%f"), m_NMEA0183.Hdg.MagneticVariationDegrees, br_var_source, br_var);
            }
            if (!wxIsNaN(m_NMEA0183.Hdg.MagneticVariationDegrees) &&
                (br_var_source <= VARIATION_SOURCE_NMEA || (br_var == 0.0 && m_NMEA0183.Hdg.MagneticVariationDegrees > 0.0))) {
                double newVar;
                if (m_NMEA0183.Hdg.MagneticVariationDirection == East) {
                    newVar = +m_NMEA0183.Hdg.MagneticVariationDegrees;
                }
                else {
                    newVar = -m_NMEA0183.Hdg.MagneticVariationDegrees;
                }
                if (fabs(newVar - br_var) >= 0.1) {
                    if (settings.verbose)wxLogMessage(wxT("BR24radar_pi: NMEA provides new magnetic variation %f"), newVar);
                }
                br_var = newVar;
                br_var_source = VARIATION_SOURCE_NMEA;
                br_var_watchdog = now;
                if (m_pMessageBox) {
                    if (m_pMessageBox->IsShown()) {
                        wxString info = _("NMEA");
                        info << wxT(" ") << br_var;
                        m_pMessageBox->SetVariationInfo(info);
                    }
                }
            }
            if (m_heading_source == HEADING_HDM && !wxIsNaN(m_NMEA0183.Hdg.MagneticSensorHeadingDegrees)) {
                br_hdt = m_NMEA0183.Hdg.MagneticSensorHeadingDegrees + br_var;
                br_hdt_watchdog = now;
            }
        }
        else if (m_heading_source == HEADING_HDM
              && m_NMEA0183.LastSentenceIDReceived == _T("HDM")
              && m_NMEA0183.Parse()
              && !wxIsNaN(m_NMEA0183.Hdm.DegreesMagnetic)) {
            br_hdt = m_NMEA0183.Hdm.DegreesMagnetic + br_var;
            br_hdt_watchdog = now;
        }
        else if (m_heading_source == HEADING_HDT
              && m_NMEA0183.LastSentenceIDReceived == _T("HDT")
              && m_NMEA0183.Parse()
              && !wxIsNaN(m_NMEA0183.Hdt.DegreesTrue)) {
            br_hdt = m_NMEA0183.Hdt.DegreesTrue;
            br_hdt_watchdog = now;
        }
    }
}


// Ethernet packet stuff *************************************************************


RadarDataReceiveThread::~RadarDataReceiveThread()
{
}

void RadarDataReceiveThread::OnExit()
{
    //  wxLogMessage(wxT("BR24radar_pi: radar thread is stopping."));
}

static int my_inet_aton(const char *cp, struct in_addr *addr)
{
    register u_long val;
    register int base, n;
    register char c;
    u_int parts[4];
    register u_int *pp = parts;

    c = *cp;
    for (;;) {
        /*
         * Collect number up to ``.''.
         * Values are specified as for C:
         * 0x=hex, 0=octal, isdigit=decimal.
         */
        if (!isdigit(c)) {
            return (0);
        }
        val = 0;
        base = 10;
        if (c == '0') {
            c = *++cp;
            if (c == 'x' || c == 'X') {
                base = 16, c = *++cp;
            } else {
                base = 8;
            }
        }
        for (;;) {
            if (isascii(c) && isdigit(c)) {
                val = (val * base) + (c - '0');
                c = *++cp;
            }
            else if (base == 16 && isascii(c) && isxdigit(c)) {
                val = (val << 4) |
                (c + 10 - (islower(c) ? 'a' : 'A'));
                c = *++cp;
            } else {
                break;
            }
        }
        if (c == '.') {
            /*
             * Internet format:
             *    a.b.c.d
             *    a.b.c    (with c treated as 16 bits)
             *    a.b    (with b treated as 24 bits)
             */
            if (pp >= parts + 3) {
                return 0;
            }
            *pp++ = val;
            c = *++cp;
        } else {
            break;
        }
    }
    /*
     * Check for trailing characters.
     */
    if (c != '\0' && (!isascii(c) || !isspace(c))) {
        return 0;
    }
    /*
     * Concoct the address according to
     * the number of parts specified.
     */
    n = pp - parts + 1;
    switch (n) {

        case 0:
            return 0;        /* initial nondigit */

        case 1:                /* a -- 32 bits */
            break;

        case 2:                /* a.b -- 8.24 bits */
            if (val > 0xffffff) {
                return 0;
            }
            val |= parts[0] << 24;
            break;

        case 3:                /* a.b.c -- 8.8.16 bits */
            if (val > 0xffff) {
                return 0;
            }
            val |= (parts[0] << 24) | (parts[1] << 16);
            break;

        case 4:                /* a.b.c.d -- 8.8.8.8 bits */
            if (val > 0xff) {
                return 0;
            }
            val |= (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8);
            break;
    }
    if (addr) {
        addr->s_addr = htonl(val);
    }
    return 1;
}

static bool socketReady( SOCKET sockfd, int timeout )
{
    int r = 0;
    fd_set fdin;
    struct timeval tv = { (long) timeout / MILLISECONDS_PER_SECOND, (long) (timeout % MILLISECONDS_PER_SECOND) * MILLISECONDS_PER_SECOND };

    FD_ZERO(&fdin);
    if (sockfd != INVALID_SOCKET) {
        FD_SET(sockfd, &fdin);
        r = select(sockfd + 1, &fdin, 0, &fdin, &tv);
    } else {
#ifndef __WXMSW__
        // Common UNIX style sleep, unlike 'sleep' this causes no alarms
        // and has fewer threading issues.
        select(1, 0, 0, 0, &tv);
#else
        Sleep(timeout);
#endif
        r = 0;
    }

    return r > 0;
}

static SOCKET startUDPMulticastReceiveSocket( br24radar_pi *pPlugIn, struct sockaddr_in * addr, UINT16 port, const char * mcastAddr)
{
    SOCKET rx_socket;
    struct sockaddr_in adr;
    int one = 1;

    if (!addr) {
        return INVALID_SOCKET;
    }

    UINT8 * a = (UINT8 *) &addr->sin_addr; // sin_addr is in network layout
    wxString address;
    address.Printf(wxT(" %u.%u.%u.%u"), a[0] , a[1] , a[2] , a[3]);

    memset(&adr, 0, sizeof(adr));
    adr.sin_family = AF_INET;
    adr.sin_addr.s_addr = htonl(INADDR_ANY); // I know, htonl is unnecessary here
    adr.sin_port = htons(port);
    rx_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (rx_socket == INVALID_SOCKET) {
        br_error_msg << _("Cannot create UDP socket");
        goto fail;
    }
    if (setsockopt(rx_socket, SOL_SOCKET, SO_REUSEADDR, (const char *) &one, sizeof(one))) {
        br_error_msg << _("Cannot set reuse address option on socket");
        goto fail;
    }

    if (bind(rx_socket, (struct sockaddr *) &adr, sizeof(adr))) {
        br_error_msg << _("Cannot bind UDP socket to port ") << port;
        goto fail;
    }

    // Subscribe rx_socket to a multicast group
    struct ip_mreq mreq;
    mreq.imr_interface = addr->sin_addr;

    if (!my_inet_aton(mcastAddr, &mreq.imr_multiaddr)) {
        br_error_msg << _("Invalid multicast address") << wxT(" ") << wxString::FromUTF8(mcastAddr);
        goto fail;
    }

    if (setsockopt(rx_socket, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char *) &mreq, sizeof(mreq))) {
        br_error_msg << _("Invalid IP address for UDP multicast");
        goto fail;
    }

    // Hurrah! Success!
    return rx_socket;

fail:
    br_error_msg << wxT(" ") << address;
    wxLogError(wxT("BR2radar_pi: %s"), br_error_msg.c_str());
    br_update_error_control = true;
    if (rx_socket != INVALID_SOCKET) {
        closesocket(rx_socket);
    }
    return INVALID_SOCKET;
}


void *RadarDataReceiveThread::Entry(void)
                                          // this thread will run twice, both for A and B
{
    SOCKET rx_socket = INVALID_SOCKET;
    int r = 0;
    sockaddr_storage rx_addr;
    socklen_t        rx_len;
    //    Loop until we quit
    while (!*m_quit) {
        if (pPlugIn->settings.emulator_on) {
            socketReady(INVALID_SOCKET, 1000); // sleep for 1s
            emulate_fake_buffer();
            if (pPlugIn->m_pMessageBox) {
                wxString ip;
                ip << _("emulator");
                pPlugIn->m_pMessageBox->SetRadarIPAddress(ip);
            }
        }
        else {
            if (rx_socket == INVALID_SOCKET) {
                if (AB == 1){         
                    rx_socket = startUDPMulticastReceiveSocket(pPlugIn, br_mcast_addr, 6657, "236.6.7.13");
                }
                else{                                                  
                    rx_socket = startUDPMulticastReceiveSocket(pPlugIn, br_mcast_addr, 6678, "236.6.7.8");
                }
                // If it is still INVALID_SOCKET now we just sleep for 1s in socketReady
                if (rx_socket != INVALID_SOCKET) {
                    wxString addr;
                    UINT8 * a = (UINT8 *) &br_mcast_addr->sin_addr; // sin_addr is in network layout
                    addr.Printf(wxT("%u.%u.%u.%u"), a[0] , a[1] , a[2] , a[3]);
                    if (pPlugIn->settings.verbose){
                        wxLogMessage(wxT("BR24radar_pi: Listening for radar AB = %d data on %s"), AB, addr.c_str());
                    }
                    my_address = a[3];
                }
            }
                if (socketReady(rx_socket, 1000)) {
                    radar_frame_pkt packet;
                    rx_len = sizeof(rx_addr);
                    r = recvfrom(rx_socket, (char *) &packet, sizeof(packet), 0, (struct sockaddr *) &rx_addr, &rx_len);
                    if (r > 0) {
                        process_buffer(&packet, r);
                    }
                    if (r < 0 || !br_mcast_addr || !br_radar_seen) {
                        closesocket(rx_socket);
                        rx_socket = INVALID_SOCKET;
                    }
                }
            
            if (!br_radar_seen || !br_mcast_addr) {
                if (rx_socket != INVALID_SOCKET) {
                    if (pPlugIn->settings.verbose){
                        wxLogMessage(wxT("BR24radar_pi: Stopped listening for radarA data"));
                    }
                    closesocket(rx_socket);
                    rx_socket = INVALID_SOCKET;
                }
            }
        }
    }

    if (rx_socket != INVALID_SOCKET) {
        closesocket(rx_socket);
    }
     
    return 0;
}

// process_buffer
// --------------
// Process one radar frame packet, which can contain up to 32 'spokes' or lines extending outwards
// from the radar up to the range indicated in the packet.
//
// We only get Data packets of fixed length from PORT (6678), see structure in .h file
//
void RadarDataReceiveThread::process_buffer(radar_frame_pkt * packet, int len)
{
    wxLongLong nowMillis = wxGetLocalTimeMillis();
    time_t now = time(0);
    br_radar_seen = true;
    br_radar_watchdog = now;
    br_data_seen = true;   // added here, otherwise loose image while data is present
    br_data_watchdog = now;
    
    static int previous_angle_raw = 0;

    // wxCriticalSectionLocker locker(br_scanLock);

    static unsigned int i_display = 0;  // used in radar reive thread for display operation
    static int next_scan_number[2] = { -1, -1 };
    int scan_number[2] = { 0, 0 };
    pPlugIn->m_statistics[AB].packets++;
    if (len < (int) sizeof(packet->frame_hdr)) {
        pPlugIn->m_statistics[AB].broken_packets++;
        return;
    }
    int scanlines_in_packet = (len - sizeof(packet->frame_hdr)) / sizeof(radar_line);
    if (scanlines_in_packet != 32) {
        pPlugIn->m_statistics[AB].broken_packets++;
    }

    for (int scanline = 0; scanline < scanlines_in_packet; scanline++) {
        radar_line * line = &packet->line[scanline];

        // Validate the spoke
        scan_number[AB] = line->br24.scan_number[0] | (line->br24.scan_number[1] << 8);
        pPlugIn->m_statistics[AB].spokes++;
        if (line->br24.headerLen != 0x18) {
            if (pPlugIn->settings.verbose) {
                wxLogMessage(wxT("BR24radar_pi: strange header length %d"), line->br24.headerLen);
            }
            // Do not draw something with this...
            pPlugIn->m_statistics[AB].missing_spokes++;
            next_scan_number[AB] = (scan_number[AB] + 1) % 4096;
            continue;
        }
        if (line->br24.status != 0x02 && line->br24.status != 0x12) {
            if (pPlugIn->settings.verbose) {
                wxLogMessage(wxT("BR24radar_pi: strange status %02x"), line->br24.status);
            }
            pPlugIn->m_statistics[AB].broken_spokes++;
        }
        if (next_scan_number[AB] >= 0 && scan_number[AB] != next_scan_number[AB]) {
            if (scan_number[AB] > next_scan_number[AB]) {
                pPlugIn->m_statistics[AB].missing_spokes += scan_number[AB] - next_scan_number[AB];
            } else {
                pPlugIn->m_statistics[AB].missing_spokes += 4096 + scan_number[AB] - next_scan_number[AB];
            }
        }
        next_scan_number[AB] = (scan_number[AB] + 1) % 4096;

        int range_raw = 0;
        int angle_raw = 0;
        short int hdm_raw = 0;
        short int large_range = 0;
        short int small_range = 0;
        int range_meters = 0;

        if (memcmp(line->br24.mark, BR24MARK, sizeof(BR24MARK)) == 0) {
            // BR24 and 3G mode
            range_raw = ((line->br24.range[2] & 0xff) << 16 | (line->br24.range[1] & 0xff) << 8 | (line->br24.range[0] & 0xff));
            angle_raw = (line->br24.angle[1] << 8) | line->br24.angle[0];
            range_meters = (int) ((double)range_raw * 10.0 / sqrt(2.0));
            br_radar_type = RT_BR24;
        } else {
            // 4G mode
            large_range = (line->br4g.largerange[1] << 8) | line->br4g.largerange[0];
            small_range = (line->br4g.smallrange[1] << 8) | line->br4g.smallrange[0];
            angle_raw = (line->br4g.angle[1] << 8) | line->br4g.angle[0];
            if (large_range == 0x80) {
                if (small_range == -1) {
                    range_raw = 0; // Invalid range received
                } else {
                    range_raw = small_range;
                }
            } else {
                range_raw = large_range * 256;
            }
            range_meters = range_raw / 4;
            br_radar_type = RT_4G;
        }
    
        previous_angle_raw = angle_raw;
       // Range change received from radar?

        if (range_meters != br_range_meters[AB]) {

            if (pPlugIn->settings.verbose >= 1) {
                if (range_meters == 0) {
                    if (pPlugIn->settings.verbose)wxLogMessage(wxT("BR24radar_pi: Invalid range received, keeping %d meters"), br_range_meters[AB]);
                }
                else {
                    if (pPlugIn->settings.verbose)wxLogMessage(wxT("BR24radar_pi: Radar now scanning with range %d meters (was %d meters)"), range_meters, br_range_meters[AB]);
                }
            }
            br_range_meters[AB] = range_meters;
            br_update_range_control[AB] = true;  // signal rendering code to change control value
        }

        hdm_raw = (line->br4g.heading[1] << 8) | line->br4g.heading[0];
        if (hdm_raw != INT16_MIN && TIMER_NOT_ELAPSED(br_var_watchdog) && br_radar_type == RT_4G) {
            br_heading_on_radar = true;                            // heading on radar
            br_hdt_raw = MOD_ROTATION(hdm_raw + SCALE_DEGREES_TO_RAW(br_var));
            br_hdt = MOD_DEGREES(SCALE_RAW_TO_DEGREES(br_hdt_raw));
            if (!blackout[AB]) angle_raw += br_hdt_raw;
        }
        else {                                // no heading on radar
            br_heading_on_radar = false;
            br_hdt_raw = SCALE_DEGREES_TO_RAW(br_hdt);
            if (!blackout[AB]) angle_raw += br_hdt_raw;             // map spoke on true direction, but in blackout head up.
        }
        // until here all is based on 4096 scanlines

        angle_raw = MOD_ROTATION2048(angle_raw / 2);   // divide by 2 to map on 2048 scanlines

        UINT8 *dest_data1 = pPlugIn->m_scan_line[AB][angle_raw].data;
        memcpy(dest_data1, line->data, RETURNS_PER_LINE);

        // now add this line to the history
        UINT8 *hist_data = pPlugIn->m_scan_line[AB][angle_raw].history;
        for (int i = 0; i < RETURNS_PER_LINE - 1; i++) {
            hist_data[i] = hist_data[i] << 1;     // shift left history byte 1 bit
            if (dest_data1[i] > displaysetting_threshold[pPlugIn->settings.display_option]) {
                hist_data[i] = hist_data[i] | 1;    // and add 1 if above threshold
            }
        }

        // The following line is a quick hack to confirm on-screen where the range ends, by putting a 'ring' of
        // returned radar energy at the max range line.
        // TODO: create nice actual range circles.
        dest_data1[RETURNS_PER_LINE - 1] = 0xff;
        pPlugIn->m_scan_line[AB][angle_raw].range = range_meters;
        pPlugIn->m_scan_line[AB][angle_raw].age = nowMillis;
        if (AB == pPlugIn->settings.selectRadarB){
            pPlugIn->PrepareRadarImage(angle_raw);   // prepare the vertex array for this line
        }                                            // but only do this for the active radar
    }


    //  all scanlines ready now, refresh section follows

    if (pPlugIn->settings.showRadar && AB == pPlugIn->settings.selectRadarB) {  // only issue refresh for active and shown channel
        int pos_age = difftime(time(0), br_bpos_watchdog);   // the age of the postion, last call of SetPositionFixEx
        if (pPlugIn->settings.display_mode[AB] == DM_CHART_BLACKOUT){  // position not important in DM_CHART_BLACKOUT
            pos_age = 0;
        }
        if (br_refresh_busy_or_queued || pos_age >= 2 ) {
            // don't do additional refresh and reset the refresh conter
            i_display = 0;  // rendering ongoing, reset the counter, don't refresh now
            // this will also balance performance, if too busy skip refresh
            // pos_age>=2 : OCPN too busy to pass position to pi, system overloaded
            // so skip next refresh
               if (pPlugIn->settings.verbose >= 2) {
            wxLogMessage(wxT("BR24radar_pi:  busy encountered, pos_age = %i, br_refresh_busy_or_queued=%i"), pos_age, br_refresh_busy_or_queued);
               }
        }
        else {
            if (i_display >= br_refresh_rate) {   //    display every "refreshrate time"
                if (br_refresh_rate != 10) { // for 10 no refresh at all
                    br_refresh_busy_or_queued = true;   // no further calls until br_refresh_busy_or_queued has been cleared by RenderGLOverlay
                    GetOCPNCanvasWindow()->Refresh(true);
                    if (pPlugIn->settings.verbose >= 4) {
                        wxLogMessage(wxT("BR24radar_pi:  refresh issued"));
                    }
                }
                i_display = 0;
            }
            i_display++;
        }
    }
}

/*
 * Called once a second. Emulate a radar return that is
 * at the current desired auto_range.
 * Speed is 24 images per minute, e.g. 1/2.5 of a full
 * image.
 */

void RadarDataReceiveThread::emulate_fake_buffer(void)
{
    wxLongLong nowMillis = wxGetLocalTimeMillis();
    time_t now = time(0);
    static int next_scan_number = 0;
    pPlugIn->m_statistics[AB].packets++;
    br_radar_seen = true;
    br_radar_watchdog = now;
    int scanlines_in_packet = 2048 * 24 / 60;
    int range_meters = br_auto_range_meters;
    int spots = 0;
    br_radar_type = RT_BR24;
    if (range_meters != br_range_meters[AB]) {
        br_range_meters[AB] = range_meters;
        // Set the control's value to the real range that we received, not a table idea
        if (pPlugIn->m_pControlDialog) {
            pPlugIn->m_pControlDialog->SetRangeIndex(convertMetersToRadarAllowedValue(&range_meters, pPlugIn->settings.range_units, br_radar_type));
        }
    }
    for (int scanline = 0; scanline < scanlines_in_packet; scanline++) {
        int angle_raw = next_scan_number;
        next_scan_number = (next_scan_number + 1) % LINES_PER_ROTATION;
        pPlugIn->m_statistics[AB].spokes++;

        // Invent a pattern. Outermost ring, then a square pattern
        UINT8 *dest_data1 = pPlugIn->m_scan_line[AB][angle_raw].data;
        for (int range = 0; range < RETURNS_PER_LINE; range++) {
            int bit = range >> 5;
            // use bit 'bit' of angle_raw
            UINT8 color = ((angle_raw >> 3) & (2 << bit)) > 0 ? 200 : 0;
            dest_data1[range] = color;
            if (color > 0) {
                spots++;
            }
        }

        // The following line is a quick hack to confirm on-screen where the range ends, by putting a 'ring' of
        // returned radar energy at the max range line.
        // TODO: create nice actual range circles.
        dest_data1[RETURNS_PER_LINE - 1] = 0xff;
        pPlugIn->m_scan_line[AB][angle_raw].range = range_meters;
        pPlugIn->m_scan_line[AB][angle_raw].age = nowMillis;
        pPlugIn->PrepareRadarImage(angle_raw);
    }
    if (pPlugIn->settings.verbose >= 2) {
        wxLogMessage(wxT("BR24radar_pi: %") wxTPRId64 wxT(" emulating %d spokes at range %d with %d spots"), nowMillis, scanlines_in_packet, range_meters, spots);
    }
}

RadarCommandReceiveThread::~RadarCommandReceiveThread()
{
}

void RadarCommandReceiveThread::OnExit()
{
}





void *RadarCommandReceiveThread::Entry(void)
{             // runs twice, both for A and B radar
    SOCKET rx_socket = INVALID_SOCKET;
    int r = 0;
    if (pPlugIn->settings.verbose)wxLogMessage(wxT(" RadarCommandReceiveThread AB = %d"), AB);
    union {
        sockaddr_storage addr;
        sockaddr_in      ipv4;
    } rx_addr;
    socklen_t        rx_len;

    //    Loop until we quit
    while (!*m_quit) {
        if (rx_socket == INVALID_SOCKET && pPlugIn->settings.emulator_on) {
            if (AB == 1){
                rx_socket = startUDPMulticastReceiveSocket(pPlugIn, br_mcast_addr, 6658, "236.6.7.14");
                //  B radar
            }
            else{
                rx_socket = startUDPMulticastReceiveSocket(pPlugIn, br_mcast_addr, 6680, "236.6.7.10");
                // socket for A radar
            }
                                         //  socket for B radar
            // If it is still INVALID_SOCKET now we just sleep for 1s in socketReady
            if (rx_socket != INVALID_SOCKET && AB == 1) {
                if (pPlugIn->settings.verbose)wxLogMessage(wxT("Listening for commands radar B socket 6658 AB = %d"), AB);
            }
            if (rx_socket != INVALID_SOCKET && AB == 0) {
                if (pPlugIn->settings.verbose)wxLogMessage(wxT("Listening for commands radar A socket 6680 AB = %d"), AB);
            }
        }
            // If it is still INVALID_SOCKET now we just sleep for 1s in socketReady
        

        if (socketReady(rx_socket, 1000)) {  // listen for commands (from ourselves or others)
            UINT8 command[1500];
            rx_len = sizeof(rx_addr);
            r = recvfrom(rx_socket, (char * ) command, sizeof(command), 0, (struct sockaddr *) &rx_addr, &rx_len);
            if (r > 0) {
                wxString s;

                if (rx_addr.addr.ss_family == AF_INET) {
                    UINT8 * a = (UINT8 *) &rx_addr.ipv4.sin_addr; // sin_addr is in network layout

                    {
                        if(pPlugIn->settings.verbose)s.Printf(wxT("%u.%u.%u.%u command received AB = %d"), a[0] , a[1] , a[2] , a[3], AB);
                    }
                } else {
                    s = wxT("non-IPV4 sent command");
                }
                if(pPlugIn->settings.verbose)logBinaryData(s, command, r);
            }
            if (r < 0 || !br_radar_seen) {
                closesocket(rx_socket);
                rx_socket = INVALID_SOCKET;
            }
        }
        else if (!br_radar_seen || !br_mcast_addr) {
            if (rx_socket != INVALID_SOCKET) {
                closesocket(rx_socket);
                rx_socket = INVALID_SOCKET;
            }
        }
    }
    if (rx_socket != INVALID_SOCKET) {
        closesocket(rx_socket);
    }
    return 0;
}

RadarReportReceiveThread::~RadarReportReceiveThread()
{
}

void RadarReportReceiveThread::OnExit()
{
}

#ifndef __WXMSW__

// Mac and Linux have ifaddrs.
# include <ifaddrs.h>
# include <net/if.h>

#else

// Emulate (just enough of) ifaddrs on Windows
// Thanks to https://code.google.com/p/openpgm/source/browse/trunk/openpgm/pgm/getifaddrs.c?r=496&spec=svn496
// Although that file has interesting new APIs the old ioctl works fine with XP and W7, and does enough
// for what we want to do.

struct ifaddrs
{
    struct ifaddrs  * ifa_next;
    struct sockaddr * ifa_addr;
    ULONG             ifa_flags;
};

struct ifaddrs_storage
{
    struct ifaddrs           ifa;
    struct sockaddr_storage  addr;
};

static int getifaddrs( struct ifaddrs ** ifap )
{
    char buf[2048];
    DWORD bytesReturned;

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        wxLogError(wxT("BR24radar_pi: Cannot get socket"));
        return -1;
    }

    if (WSAIoctl(sock, SIO_GET_INTERFACE_LIST, 0, 0, buf, sizeof(buf), &bytesReturned, 0, 0) < 0) {
        wxLogError(wxT("BR24radar_pi: Cannot get interface list"));
        closesocket(sock);
        return -1;
    }

    /* guess return structure from size */
    unsigned iilen;
    INTERFACE_INFO *ii;
    INTERFACE_INFO_EX *iix;

    if (0 == bytesReturned % sizeof(INTERFACE_INFO)) {
        iilen = bytesReturned / sizeof(INTERFACE_INFO);
        ii    = (INTERFACE_INFO*) buf;
        iix   = NULL;
    } else {
        iilen  = bytesReturned / sizeof(INTERFACE_INFO_EX);
        ii     = NULL;
        iix    = (INTERFACE_INFO_EX*)buf;
    }

    /* alloc a contiguous block for entire list */
    unsigned n = iilen, k =0;
    struct ifaddrs_storage * ifa = (struct ifaddrs_storage *) malloc(n * sizeof(struct ifaddrs_storage));
    memset(ifa, 0, n * sizeof(struct ifaddrs_storage));

    /* foreach interface */
    struct ifaddrs_storage * ift = ifa;

    for (unsigned i = 0; i < iilen; i++) {
        ift->ifa.ifa_addr = (sockaddr *) &ift->addr;
        if (ii) {
            memcpy (ift->ifa.ifa_addr, &ii[i].iiAddress.AddressIn, sizeof(struct sockaddr_in));
            ift->ifa.ifa_flags = ii[i].iiFlags;
        } else {
            memcpy (ift->ifa.ifa_addr, iix[i].iiAddress.lpSockaddr, iix[i].iiAddress.iSockaddrLength);
            ift->ifa.ifa_flags = iix[i].iiFlags;
        }

        k++;
        if (k < n) {
            ift->ifa.ifa_next = (struct ifaddrs *)(ift + 1);
            ift = (struct ifaddrs_storage *)(ift->ifa.ifa_next);
        }
    }

    *ifap = (struct ifaddrs*) ifa;
    closesocket(sock);
    return 0;
}

void freeifaddrs( struct ifaddrs *ifa)
{
    free(ifa);
}

#endif

#define VALID_IPV4_ADDRESS(i) \
(  i \
&& i->ifa_addr \
&& i->ifa_addr->sa_family == AF_INET \
&& (i->ifa_flags & IFF_UP) > 0 \
&& (i->ifa_flags & IFF_LOOPBACK) == 0 \
&& (i->ifa_flags & IFF_MULTICAST) > 0 )

void *RadarReportReceiveThread::Entry(void)
{
    SOCKET rx_socket = INVALID_SOCKET;
    int r = 0;
    int count = 0;
    if (pPlugIn->settings.verbose)wxLogMessage(wxT("RadarReportReceiveThread AB = %d Entry"), AB);

    // This thread is special as it is the only one that loops round over the interfaces
    // to find the radar

    struct ifaddrs * ifAddrStruct;
    struct ifaddrs * ifa;
    static struct sockaddr_in mcastFoundAddr;
    static struct sockaddr_in radarFoundAddr;

    sockaddr_storage rx_addr;
    socklen_t        rx_len;

    ifAddrStruct = 0;
    ifa = 0;

    if (pPlugIn->settings.verbose) {
        wxLogMessage(wxT("BR24radar_pi: Listening for reports"));
    }
    //    Loop until we quit

    while (!*m_quit) {
        if (AB == 0){     // radar A
            if (rx_socket == INVALID_SOCKET && !pPlugIn->settings.emulator_on) {
                // Pick the next ethernet card

                // If set, we used this one last time. Go to the next card.
                if (ifa) {
                    ifa = ifa->ifa_next;
                }
                // Loop until card with a valid IPv4 address
                while (ifa && !VALID_IPV4_ADDRESS(ifa)) {
                    ifa = ifa->ifa_next;
                }
                if (!ifa) {
                    if (ifAddrStruct) {
                        freeifaddrs(ifAddrStruct);
                        ifAddrStruct = 0;
                    }
                    if (!getifaddrs(&ifAddrStruct)) {
                        ifa = ifAddrStruct;
                    }
                    // Loop until card with a valid IPv4 address
                    while (ifa && !VALID_IPV4_ADDRESS(ifa)) {
                        ifa = ifa->ifa_next;
                    }
                }
                if (VALID_IPV4_ADDRESS(ifa)) {
                    rx_socket = startUDPMulticastReceiveSocket(pPlugIn, (struct sockaddr_in *)ifa->ifa_addr, 6679, "236.6.7.9");
                    if (rx_socket != INVALID_SOCKET) {
                        wxString addr;
                        UINT8 * a = (UINT8 *)&((struct sockaddr_in *)ifa->ifa_addr)->sin_addr; // sin_addr is in network layout
                        addr.Printf(wxT("%u.%u.%u.%u"), a[0], a[1], a[2], a[3]);
                        if (pPlugIn->settings.verbose >= 1) {
                            wxLogMessage(wxT("BR24radar_pi: Listening for radarA reports on %s"), addr.c_str());
                        }
                        br_ip_address = addr;
                        br_update_address_control = true;    //signals to RenderGLOverlay that the control box should be updated
                        count = 0;
                    }
                }
            }
        }                        //  end of radar A 
            else
            {          // radar B
                if (br_mcast_addr != 0 && rx_socket == INVALID_SOCKET && !pPlugIn->settings.emulator_on){
                    rx_socket = startUDPMulticastReceiveSocket(pPlugIn, br_mcast_addr, 6659, "236.6.7.15");
                    if (rx_socket != INVALID_SOCKET) {
                    //    wxString addr;
                    //    UINT8 * a = (UINT8 *)&((struct sockaddr_in *)ifa->ifa_addr)->sin_addr; // sin_addr is in network layout
                    //    addr.Printf(wxT("%u.%u.%u.%u"), a[0], a[1], a[2], a[3]);
                        //    if (pPlugIn->settings.verbose >= 1) {
                        if (pPlugIn->settings.verbose)wxLogMessage(wxT("BR24radar_pi:  AB = 1 Listening for radarB reports "));
                        //    }
                        //         br_ip_address = addr;
                        //         br_update_address_control = true;    //signals to RenderGLOverlay that the control box should be updated
                        count = 0;
                    }
                }
            }        // end of radar B
        
            // If it is still INVALID_SOCKET now we just sleep for 1s in socketReady

        if (socketReady(rx_socket, 1000)) {
            UINT8 report[1500];
            rx_len = sizeof(rx_addr);
            r = recvfrom(rx_socket, (char * ) report, sizeof(report), 0, (struct sockaddr *) &rx_addr, &rx_len);
            if (r > 0) {

                if (ProcessIncomingReport(report, r) && AB == 0) {    // NB for AB == 1 ifa is not set
                    memcpy(&mcastFoundAddr, ifa->ifa_addr, sizeof(mcastFoundAddr));
                    br_mcast_addr = &mcastFoundAddr;
                    memcpy(&radarFoundAddr, &rx_addr, sizeof(radarFoundAddr));
                    br_radar_addr = &radarFoundAddr;
                    wxString addr;
                    UINT8 * a = (UINT8 *) &br_radar_addr->sin_addr; // sin_addr is in network layout
                    addr.Printf(wxT("%u.%u.%u.%u"), a[0] , a[1] , a[2] , a[3]);
                    br_ip_address = addr;
                    br_update_address_control = true;   //signals to RenderGLOverlay that the control box should be updated
                    if (!br_radar_seen && AB == 0) {
                        if (pPlugIn->settings.verbose)wxLogMessage(wxT("BR24radar_pi: detected radar A at %s"), addr.c_str());
                    }
                        br_radar_seen = true;
                        br_radar_watchdog = time(0);
                }
            }

            if ((r < 0 ) || !br_radar_seen) { // on error, or if we haven't received anything we start looping again
                closesocket(rx_socket);
                rx_socket = INVALID_SOCKET;
                if (AB == 0) {
                    br_mcast_addr = 0;
                    br_radar_addr = 0;
                }
            }

        } else if (count >= 2 && !br_radar_seen && rx_socket != INVALID_SOCKET) {
            closesocket(rx_socket);
            rx_socket = INVALID_SOCKET;
            br_mcast_addr = 0;
            br_radar_addr = 0;
        } else {
            count++;
        }
    }   // end of while

    if (rx_socket != INVALID_SOCKET) {
        closesocket(rx_socket);
    }
    
    if (ifAddrStruct) {
        freeifaddrs(ifAddrStruct);
    }
    return 0;
}

//
// The following is the received radar state. It sends this regularly
// but especially after something sends it a state change.
//
#pragma pack(push,1)
struct radar_state02 {
    UINT8  what;     // 0   0x02
    UINT8  command;  // 1 0xC4
    UINT16 range;    //  2-3   0x06 0x09
    UINT32 field4;   // 4-7    0
    UINT32 field8;   // 8-11
    UINT8  gain;     // 12
    UINT8  field13;  // 13  ==1 for sea auto
    UINT8  field14;  // 14
    UINT16 field15;  // 15-16
    UINT32 sea;      // 17-20   sea state (17)
    UINT8  field21;  // 21 
    UINT8  rain;     // 22   rain clutter
    UINT8  field23;  // 23 
    UINT32 field24;  //24-27
    UINT32 field28;  //28-31
    UINT8  field32;  //32
    UINT8  field33;  //33
    UINT8  interference_rejection;  //34
    UINT8  field35;  //35
    UINT8  field36;  //36
    UINT8  field37;  //37
    UINT8  field38;  //38
    UINT8  field39;  //39
    UINT8  field40;  //40
    UINT8  field41;  //41
    UINT8  target_boost;  //42
    UINT16 field8a;
    UINT32 field8b;
    UINT32 field9;
    UINT32 field10;
    UINT32 field11;
    UINT32 field12;
    UINT32 field13x;
    UINT32 field14x;
};

struct radar_state04_66 {  // 04 C4 with length 66
    UINT8  what;     // 0   0x04
    UINT8  command;  // 1   0xC4
    UINT32 field2;   // 2-5
    UINT16 bearing_alignment;    // 6-7
    UINT16 field8;   // 8-9
    UINT16 antenna_height;       // 10-11
};

struct radar_state01_18 {  // 04 C4 with length 66
    UINT8  what;      // 0   0x01
    UINT8  command;   // 1   0xC4
    UINT8  radar_status;    // 2
    UINT8  field3;    // 3
    UINT8  field4;    // 4
    UINT8  field5;    // 5
    UINT16 field6;    // 6-7
    UINT16 field8;    // 8-9
    UINT16 field10;   // 10-11
};

struct radar_state08_18 {  //08 c4  length 18
    UINT8  what;        // 0  0x08   
    UINT8  command;     // 1  0xC4      
    UINT8  field2;      // 2
    UINT8  local_interference_rejection;   // 3
    UINT8  scan_speed;  // 4
    UINT8  sls_auto;    // 5 installation: sidelobe suppression auto
    UINT8  field6;      // 6
    UINT8  field7;      // 7
    UINT8  field8;      // 8
    UINT8  side_lobe_suppression;      // 9 installation: sidelobe suppression
    UINT16 field10;     // 10-11          
    UINT8  noise_rejection;       // 12    noise rejection    
    UINT8  target_sep;  // 13
};
#pragma pack(pop)

bool RadarReportReceiveThread::ProcessIncomingReport( UINT8 * command, int len )
{
    static char prevStatus = 0;
    if (pPlugIn->settings.verbose)wxLogMessage(wxT("BR24radar_pi: report received AB = %d"), AB);
    if (pPlugIn->settings.verbose)logBinaryData(wxT("report received "), command, len);
    if (command[1] == 0xC4) {

        // Looks like a radar report. Is it a known one?
        switch ((len << 8) + command[0]) {

        case (18 << 8) + 0x01: { //  length 18, 01 C4
            radar_state01_18 * s = (radar_state01_18 *)command;
            // Radar status in byte 2
            if (s->radar_status != prevStatus) {
                //       if (pPlugIn->settings.verbose > 0) {
                {
                    if (pPlugIn->settings.verbose)wxLogMessage(wxT("BR24radar_pi: process inc report radar AB = %d status = %u"), AB, command[2]);
                }
                prevStatus = command[2];
                if (AB == 1) br_radar_type = RT_4G;   // only 4G Tx on channel B
            }
            

            break;
        }

        case (99 << 8) + 0x02:   // length 99, 08 C4
        {
            radar_state02 * s = (radar_state02 *)command;
            if (s->field8 == 1){   // 1 for auto
                pPlugIn->radar_setting[AB].gain.Update(-1);  // auto gain
            }
            else{
                pPlugIn->radar_setting[AB].gain.Update(s->gain * 100 / 255);
            } // is handled elsewhere
            //            pPlugIn->radar_setting[AB].range.Update(idx);
            pPlugIn->radar_setting[AB].rain.Update(s->rain * 100 / 255);
            if (s->field13 == 0x01){
                pPlugIn->radar_setting[AB].sea.Update(-1); // auto sea
            }
            else{
                pPlugIn->radar_setting[AB].sea.Update(s->sea * 100 / 255);
            }
            pPlugIn->radar_setting[AB].target_boost.Update(s->target_boost);
            pPlugIn->radar_setting[AB].interference_rejection.Update(s->interference_rejection);

            if (pPlugIn->settings.verbose)wxLogMessage(wxT("BR24radar_pi: radar AB = %d state range=%u gain=%u sea=%u rain=%u interference_rejection=%u target_boost=%u ")
                , AB
                , s->range
                , s->gain
                , s->sea
                , s->rain
                , s->interference_rejection
                , s->target_boost
                );
            //       logBinaryData(wxT("state"), command, len);
        }
        break;

        case (564 << 8) + 0x05:    // length 564, 05 C4
            {
            // Content unknown, but we know that BR24 radomes send this
            if (pPlugIn->settings.verbose)logBinaryData(wxT("received familiar 3G report"), command, len);
            br_radar_type = RT_BR24;
            break;
            }

        case (18 << 8) + 0x08:   // length 18, 08 C4
        {
            // contains scan speed, noise rejection and target_separation and sidelobe suppression
            radar_state08_18 * s08 = (radar_state08_18 *)command;

            if (pPlugIn->settings.verbose)wxLogMessage(wxT("BR24radar_pi: radar AB = %d scanspeed= %d, noise = %u target_sep %u"), AB, s08->scan_speed, s08->noise_rejection, s08->target_sep);
            if (pPlugIn->settings.verbose)logBinaryData(wxT("received report_08"), command, len);
            pPlugIn->radar_setting[AB].scan_speed.Update(s08->scan_speed);
            pPlugIn->radar_setting[AB].noise_rejection.Update(s08->noise_rejection);
            pPlugIn->radar_setting[AB].target_separation.Update(s08->target_sep);
                if (s08->sls_auto == 1){
                    pPlugIn->radar_setting[AB].side_lobe_suppression.Update(-1);
                }
                else{
                    pPlugIn->radar_setting[AB].side_lobe_suppression.Update(s08->side_lobe_suppression * 100 / 255);
                }
// local interference rejection
            pPlugIn->radar_setting[AB].local_interference_rejection.Update(s08->local_interference_rejection);
                
            if (pPlugIn->settings.verbose)wxLogMessage(wxT("BR24radar_pi: receive report AB= %d"), AB);
            if (pPlugIn->settings.verbose)logBinaryData(wxT("received report_08"), command, len);
            break;
        }

        case (66 << 8) + 0x04:     // 66 bytes starting with 04 C4
        {
            if (pPlugIn->settings.verbose)logBinaryData(wxT("received report_04 - 66"), command, len);
            radar_state04_66 * s04_66 = (radar_state04_66 *)command;

            // bearing alignment
            int ba = (int)s04_66->bearing_alignment / 10;
            if (ba > 180){
                ba = ba - 360;
            }
            pPlugIn->radar_setting[AB].bearing_alignment.Update(ba);

            // antenna height
            pPlugIn->radar_setting[AB].antenna_height.Update(s04_66->antenna_height / 1000);
            break;
        }

        default:
            //      if (pPlugIn->settings.verbose >= 2) {
        {
        if(pPlugIn->settings.verbose)    logBinaryData(wxT("received unknown report"), command, len);
        }
        break;

        }
        return true;
    }
    if (command[1] == 0xF5) {
        // Looks like a radar report. Is it a known one?
        switch ((len << 8) + command[0]) {
        case (16 << 8) + 0x0f:
            if (pPlugIn->settings.verbose)logBinaryData(wxT("received 3G report"), command, len);
            br_radar_type = RT_BR24;
            break;

        case (8 << 8) + 0x10:
        case (10 << 8) + 0x12:
        case (46 << 8) + 0x13:
            // Content unknown, but we know that BR24 radomes send this
            //       if (pPlugIn->settings.verbose >= 4) {
        {
            if (pPlugIn->settings.verbose)logBinaryData(wxT("received familiar report "), command, len);
        }
        break;

        default:
            //           if (pPlugIn->settings.verbose >= 2) {
        {
            if (pPlugIn->settings.verbose)logBinaryData(wxT("received unknown report "), command, len);
        }
        break;

        }
        return true;
    }
    //   if (pPlugIn->settings.verbose >= 2) {
    {
        if (pPlugIn->settings.verbose)logBinaryData(wxT("received unknown message "), command, len);
    }
    return false;
}


// vim: sw=4:ts=8:
