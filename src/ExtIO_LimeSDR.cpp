/*
The MIT License

Copyright(c) 2017 Jiang Wei  <jiangwei@jiangwei.org>
New and modified work Copyright(c) 2018 Lime Microsystems Ltd

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files(the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions :

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE
*/

/*---------------------------------------------------------------------------*/
#define EXTIO_EXPORTS		1
#define HWNAME				"ExtIO_Lime"
#define HWMODEL				"ExtIO_Lime"
#define VERNUM              "1.06"
#define EXT_BLOCKLEN		4096			/* only multiples of 512 */
//---------------------------------------------------------------------------
#include "ExtIO_LimeSDR.h"
#include <windows.h>
#include <Windowsx.h>
#include <string>
#include <commctrl.h>
#include <LimeSuite.h>
#include "resource.h"
#include <process.h>
//---------------------------------------------------------------------------

#ifdef _DEBUG
#define _MYDEBUG // Activate a debug console
#endif

#ifdef  _MYDEBUG
/* Debug Trace Enabled */
#include <stdio.h>
#define DbgPrintf printf
#else
/* Debug Trace Disabled */
#define DbgPrintf(Message) MessageBox(NULL, Message, NULL, MB_OK|MB_ICONERROR) 
#endif

#pragma warning(disable : 4996)
#define snprintf	_snprintf
//---------------------------------------------------------------------------
float_type sampleRates[] =
{
    2000000,
    4000000,
    8000000,
    10000000,
    15000000,
    20000000,
    25000000,
    30000000
};

enum CalibrationStatus {
    Calibrated = 1,
    NotCalibrated = 0,
    CalibrationErr = -1
};

int     isCalibrated = NotCalibrated;
bool	isRunning = false;
bool    LPFenable = false;
bool error_enabled = true;
char lastUsedDeviceName[32];
pfnExtIOCallback ExtIOCallback = nullptr;
int16_t* buffer = nullptr;

lms_stream_t streamId;
lms_info_str_t deviceList[10];
lms_device_t * device = nullptr;
LMS_LogHandler log_handler = nullptr;

HANDLE thread_handle = INVALID_HANDLE_VALUE;
EXTERN_C IMAGE_DOS_HEADER __ImageBase;
HWND h_dialog = nullptr;
HWND error_dialog = nullptr;

std::string DeviceInfo[10][10];
int currentDeviceIndex = 0;
int numOfDevices = 0;
int numOfChannels = 0;
size_t channel = 0;
size_t oversample = 2;
int sr_idx = 1;
int ant_select = LMS_PATH_AUTO;
int64_t CurrentLOfreq = 28.5e6;     // default HDSDR LO freq
float_type LPFbandwidth = sampleRates[sr_idx];
float_type CalibrationBandwidth = sampleRates[sr_idx];

/* 53 dB gain */
uint16_t LNA = 10;
uint16_t TIA = 3;
uint16_t PGA = 12;
//---------------------------------------------------------------------------
static void RecvThread(void *p)
{
    while (isRunning) {

        int samplesRead = LMS_RecvStream(&streamId, buffer, EXT_BLOCKLEN, NULL, 1000);
        if (ExtIOCallback != nullptr) {
            ExtIOCallback(samplesRead * 2, 0, 0, buffer);
        }
    }
    _endthread();
}
//---------------------------------------------------------------------------
static void error(int lvl, const char *msg)
{
#ifdef _MYDEBUG
    DbgPrintf(msg);
    DbgPrintf("\n");
#else
    if (error_enabled && lvl < 2)
    {
        if (lvl == 1) {
            ExtIOCallback(-1, extHw_Stop, 0, NULL);
        }
        DbgPrintf(msg);
    }
#endif
}
//---------------------------------------------------------------------------
static int SetGain()
{
    int rcc_ctl_pga_rbb = (430 * (pow(0.65, ((double)PGA / 10))) - 110.35) / 20.4516 + 16; //From datasheet

    if (LMS_WriteParam(device, LMS7param(G_LNA_RFE), LNA) != 0
        || LMS_WriteParam(device, LMS7param(G_PGA_RBB), PGA) != 0
        || LMS_WriteParam(device, LMS7param(RCC_CTL_PGA_RBB), rcc_ctl_pga_rbb) != 0
        || LMS_WriteParam(device, LMS7param(G_TIA_RFE), TIA) != 0) {
        return -1;
    }
    return 0;
}
//---------------------------------------------------------------------------
static void PerformCalibration(bool enable_error)
{
    int64_t freq = -1;
    if (isRunning) {
        freq = GetHWLO64();
        StopHW();
    }
    error_enabled = enable_error;

    isCalibrated = Calibrated;
    if (LMS_Calibrate(device, LMS_CH_RX, channel, CalibrationBandwidth, 0) != 0) {
        isCalibrated = CalibrationErr;
    }
    
    error_enabled = true;
    if (freq != -1)
        StartHW64(freq);
}
//---------------------------------------------------------------------------
static int DisableLPF()
{
    int64_t freq = -1;
    if (isRunning) {
        freq = GetHWLO64();
        StopHW();
    }

    /* Making sure that tia isn't -12dB */
    if (LMS_WriteParam(device, LMS7param(G_TIA_RFE), 3) != 0) {
        return -1;
    }

    /* If the bandwidth is higher than 110e6, LPF is bypassed */
    if (LMS_SetLPFBW(device, LMS_CH_RX, channel, 130e6) != 0) {
        return -1;
    }

    LPFenable = false;

    if (LMS_WriteParam(device, LMS7param(G_TIA_RFE), TIA) != 0) {
        return -1;
    }
    if (freq != -1)
        StartHW64(freq);

    return 0;
}
//---------------------------------------------------------------------------
static int EnableLPF()
{
    if (LPFbandwidth > 1.4e6 && LPFbandwidth <= 130e6) {
        int64_t freq = -1;

        if (isRunning) {
            freq = GetHWLO64();
            StopHW();
        }

        if (LMS_WriteParam(device, LMS7param(G_TIA_RFE), 3) != 0) {
            return -1;
        }

        if (LMS_SetLPFBW(device, LMS_CH_RX, channel, LPFbandwidth) != 0) {
            return -1;
        }

        if (LMS_WriteParam(device, LMS7param(G_TIA_RFE), TIA) != 0) {
            return -1;
        }
        LPFenable = true;

        if (LPFbandwidth > 2.5e6)
            CalibrationBandwidth = LPFbandwidth;
        else
            CalibrationBandwidth = 2.5e6;


        if (freq != -1)
            StartHW64(freq);
    }
    else {
        DbgPrintf("RxLPF frequency out of range, available range from 1.4 to 130 MHz");
        return -1;
    }

    return 0;
}
//---------------------------------------------------------------------------
static int InitializeLMS()
{
    if (LMS_Open(&device, deviceList[currentDeviceIndex], NULL) != LMS_SUCCESS) {
        return -1;
    }

    numOfChannels = LMS_GetNumChannels(device, LMS_CH_RX);
    if (numOfChannels == -1) {
        return -1;
    }

    if (LMS_Init(device) != LMS_SUCCESS) {
        return -1;
    }
    if (LMS_SetSampleRate(device, sampleRates[sr_idx], oversample) != LMS_SUCCESS) {
        return -1;
    }

    if (LMS_EnableChannel(device, LMS_CH_RX, channel, true) != LMS_SUCCESS) {
        return -1;
    }
    /* TX channel needs to be enabled for LPF and calibration */
    if (LMS_EnableChannel(device, LMS_CH_TX, channel, true) != LMS_SUCCESS) {
        return -1;
    }
    if (LMS_SetAntenna(device, LMS_CH_RX, channel, ant_select) != LMS_SUCCESS) {
        return -1;
    }

    if (LPFenable) {
        if (EnableLPF() != 0)
            return -1;
    }
    else {
        if (DisableLPF() != 0)
            return -1;
    }
    if (SetGain() != 0)
        return -1;

    if (LMS_SetLOFrequency(device, LMS_CH_RX, channel, float_type(CurrentLOfreq)) != LMS_SUCCESS) {
        return -1;
    }
    PerformCalibration(false);

    return 0;
}

//---------------------------------------------------------------------------
void UpdateDialog()
{
    Button_SetCheck(GetDlgItem(h_dialog, IDC_CHECK_ALPF), LPFenable);   // LPF checkbox
    ComboBox_SetCurSel(GetDlgItem(h_dialog, IDC_COMBO_CHAN), channel);  // Channel dropdown
    ComboBox_SetCurSel(GetDlgItem(h_dialog, IDC_COMBO_DEVICE), currentDeviceIndex);// Device dropdown

    /* Set antenna selection */
    for (int i = 0; i < ComboBox_GetCount(GetDlgItem(h_dialog, IDC_COMBO_ANT)); i++)
        if (ComboBox_GetItemData(GetDlgItem(h_dialog, IDC_COMBO_ANT), i) == ant_select)
            ComboBox_SetCurSel(GetDlgItem(h_dialog, IDC_COMBO_ANT), i);

    /* Update LNA slider */
    LMS_ReadParam(device, LMS7param(G_LNA_RFE), &LNA);
    SendDlgItemMessage(h_dialog, IDC_SLIDER_LNA, TBM_SETPOS, TRUE, 16 - ((int)LNA));
    std::string lna_value = std::to_string(LNA > 8 ? (LNA - 15) : (LNA - 11) * 3);
    lna_value.append(" dB");
    Static_SetText(GetDlgItem(h_dialog, IDC_TEXT_LNA), lna_value.c_str());

    /* Update TIA slider */
    LMS_ReadParam(device, LMS7param(G_TIA_RFE), &TIA);
    SendDlgItemMessage(h_dialog, IDC_SLIDER_TIA, TBM_SETPOS, TRUE, 4 - ((int)TIA));
    std::string tia_value = std::to_string((TIA == 3) ? 0 : (TIA == 2) ? -3 : -12);
    tia_value.append(" dB");
    Static_SetText(GetDlgItem(h_dialog, IDC_TEXT_TIA), tia_value.c_str());

    /* Update PGA slider */
    LMS_ReadParam(device, LMS7param(G_PGA_RBB), &PGA);
    SendDlgItemMessage(h_dialog, IDC_SLIDER_PGA, TBM_SETPOS, TRUE, 31 - ((int)PGA));
    std::string pga_value = std::to_string(PGA - 12);
    pga_value.append(" dB");
    Static_SetText(GetDlgItem(h_dialog, IDC_TEXT_PGA), pga_value.c_str());

    /* Update LPF bandwidth */
    char bandwidth[7];
    sprintf(bandwidth, "%3.2f", LPFbandwidth / 1e6);
    SetDlgItemText(h_dialog, IDC_ALPF_BW, bandwidth);

    /* Update calibration bandwidth */
    sprintf(bandwidth, "%3.2f", CalibrationBandwidth / 1e6);
    SetDlgItemText(h_dialog, IDC_CAL_BW, bandwidth);

    /* Update calibration text */
    if (isCalibrated == Calibrated)
        Static_SetText(GetDlgItem(h_dialog, IDC_TEXT_CALIBRATED), "Calibrated");
    else if (isCalibrated == NotCalibrated)
        Static_SetText(GetDlgItem(h_dialog, IDC_TEXT_CALIBRATED), "Not calibrated");
    else if (isCalibrated == CalibrationErr)
        Static_SetText(GetDlgItem(h_dialog, IDC_TEXT_CALIBRATED), "Calibration failed");
}
//---------------------------------------------------------------------------
static INT_PTR CALLBACK MainDlgProc(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{

    switch (uMsg) {
        /* Init starting variables */
    case WM_INITDIALOG:
    {
        /* Add device choices */
        for (int i = 0; i < numOfDevices; i++) {
            std::string info = std::to_string(i + 1) + ". " + DeviceInfo[i][0] + " (" + DeviceInfo[i][1] + ")";
            ComboBox_AddString(GetDlgItem(hwndDlg, IDC_COMBO_DEVICE), info.c_str());
        }
        ComboBox_SetCurSel(GetDlgItem(hwndDlg, IDC_COMBO_DEVICE), currentDeviceIndex);


        /* Add antenna choices */
        if (DeviceInfo[currentDeviceIndex][0] == "LimeSDR Mini") {
            ComboBox_AddString(GetDlgItem(hwndDlg, IDC_COMBO_ANT), "Auto");
            ComboBox_SetItemData(GetDlgItem(hwndDlg, IDC_COMBO_ANT), 0, 255);
            ComboBox_AddString(GetDlgItem(hwndDlg, IDC_COMBO_ANT), "LNA_H");
            ComboBox_SetItemData(GetDlgItem(hwndDlg, IDC_COMBO_ANT), 1, 1);
            ComboBox_AddString(GetDlgItem(hwndDlg, IDC_COMBO_ANT), "LNA_W");
            ComboBox_SetItemData(GetDlgItem(hwndDlg, IDC_COMBO_ANT), 2, 3);
        } else if (DeviceInfo[currentDeviceIndex][0] == "LimeNET-Micro") {
            ComboBox_AddString(GetDlgItem(hwndDlg, IDC_COMBO_ANT), "Auto");
            ComboBox_SetItemData(GetDlgItem(hwndDlg, IDC_COMBO_ANT), 0, 255);
            ComboBox_AddString(GetDlgItem(hwndDlg, IDC_COMBO_ANT), "LNA_H");
            ComboBox_SetItemData(GetDlgItem(hwndDlg, IDC_COMBO_ANT), 1, 1);
            ComboBox_AddString(GetDlgItem(hwndDlg, IDC_COMBO_ANT), "LNA_L");
            ComboBox_SetItemData(GetDlgItem(hwndDlg, IDC_COMBO_ANT), 2, 2);
        } else {
            ComboBox_AddString(GetDlgItem(hwndDlg, IDC_COMBO_ANT), "LNA_H");
            ComboBox_SetItemData(GetDlgItem(hwndDlg, IDC_COMBO_ANT), 0, 1);
            ComboBox_AddString(GetDlgItem(hwndDlg, IDC_COMBO_ANT), "LNA_L");
            ComboBox_SetItemData(GetDlgItem(hwndDlg, IDC_COMBO_ANT), 1, 2);
            ComboBox_AddString(GetDlgItem(hwndDlg, IDC_COMBO_ANT), "LNA_W");
            ComboBox_SetItemData(GetDlgItem(hwndDlg, IDC_COMBO_ANT), 2, 3);
        }


        /* Add channel choices */
        for (int i = 0; i < numOfChannels; i++) {
            std::string channels = "RX" + std::to_string(i + 1);
            ComboBox_AddString(GetDlgItem(hwndDlg, IDC_COMBO_CHAN), channels.c_str());
        }

        /* Add tickmarks, set range for LNA slider */
        SendDlgItemMessage(hwndDlg, IDC_SLIDER_LNA, TBM_SETRANGEMIN, FALSE, 1);
        SendDlgItemMessage(hwndDlg, IDC_SLIDER_LNA, TBM_SETRANGEMAX, FALSE, 15);
        for (int i = 0; i < 15; i++) {
            SendDlgItemMessage(hwndDlg, IDC_SLIDER_LNA, TBM_SETTIC, FALSE, i);
        }

        /* Add tickmarks, set range for TIA slider */
        SendDlgItemMessage(hwndDlg, IDC_SLIDER_TIA, TBM_SETRANGEMIN, FALSE, 1);
        SendDlgItemMessage(hwndDlg, IDC_SLIDER_TIA, TBM_SETRANGEMAX, FALSE, 3);
        for (int i = 0; i < 3; i++) {
            SendDlgItemMessage(hwndDlg, IDC_SLIDER_TIA, TBM_SETTIC, FALSE, i);
        }

        /* Add tickmarks, set range for PGA slider */
        SendDlgItemMessage(hwndDlg, IDC_SLIDER_PGA, TBM_SETRANGEMIN, FALSE, 0);
        SendDlgItemMessage(hwndDlg, IDC_SLIDER_PGA, TBM_SETRANGEMAX, FALSE, 31);
        for (int i = 0; i < 31; i++) {
            SendDlgItemMessage(hwndDlg, IDC_SLIDER_PGA, TBM_SETTIC, FALSE, i);
        }

        /* Add library version */
        Static_SetText(GetDlgItem(hwndDlg, IDC_TEXT_LIBVER), LMS_GetLibraryVersion());
        /* Add ExtIO version */
        Static_SetText(GetDlgItem(hwndDlg, IDC_TEXT_EXTVER), VERNUM);

        UpdateDialog();
        return TRUE;
    }
    break;

    /* Update dialog */
    case WM_SHOWWINDOW:
    {
        UpdateDialog();
        return TRUE;
    }
    break;
    /* Scroll message */
    case WM_VSCROLL:
    {
        /* LNA slider moved */
        if (GetDlgItem(hwndDlg, IDC_SLIDER_LNA) == (HWND)lParam) {
            if (LNA != 16 - SendDlgItemMessage(hwndDlg, IDC_SLIDER_LNA, TBM_GETPOS, 0, NULL)) {
                LNA = 16 - SendDlgItemMessage(hwndDlg, IDC_SLIDER_LNA, TBM_GETPOS, 0, NULL);
                std::string lna_value = std::to_string(LNA > 8 ? (LNA - 15) : (LNA - 11) * 3); // Calculate from index to dB
                lna_value.append(" dB");
                Static_SetText(GetDlgItem(hwndDlg, IDC_TEXT_LNA), lna_value.c_str());

                if (LNA <= 15)
                    LMS_WriteParam(device, LMS7param(G_LNA_RFE), LNA);

                ExtIOCallback(-1, extHw_Changed_ATT, 0, NULL);
                isCalibrated = NotCalibrated;
                UpdateDialog();
                return TRUE;
            }
        }
        /* TIA slider moved */
        if (GetDlgItem(hwndDlg, IDC_SLIDER_TIA) == (HWND)lParam) {
            if (TIA != 4 - SendDlgItemMessage(hwndDlg, IDC_SLIDER_TIA, TBM_GETPOS, 0, NULL)) {
                TIA = 4 - SendDlgItemMessage(hwndDlg, IDC_SLIDER_TIA, TBM_GETPOS, 0, NULL);
                std::string tia_value = std::to_string((TIA == 3) ? 0 : (TIA == 2) ? -3 : -12); // Calculate from index to dB
                tia_value.append(" dB");
                Static_SetText(GetDlgItem(hwndDlg, IDC_TEXT_TIA), tia_value.c_str());

                if (TIA <= 3)
                    LMS_WriteParam(device, LMS7param(G_TIA_RFE), TIA);

                ExtIOCallback(-1, extHw_Changed_ATT, 0, NULL);
                isCalibrated = NotCalibrated;
                UpdateDialog();
                return TRUE;
            }
        }
        /* PGA slider moved */
        if (GetDlgItem(hwndDlg, IDC_SLIDER_PGA) == (HWND)lParam) {
            if (PGA != 31 - SendDlgItemMessage(hwndDlg, IDC_SLIDER_PGA, TBM_GETPOS, 0, NULL)) {
                PGA = 31 - SendDlgItemMessage(hwndDlg, IDC_SLIDER_PGA, TBM_GETPOS, 0, NULL);
                std::string pga_value = std::to_string(PGA - 12); // Calculate from index to dB
                pga_value.append(" dB");
                Static_SetText(GetDlgItem(hwndDlg, IDC_TEXT_PGA), pga_value.c_str());

                int rcc_ctl_pga_rbb = (430 * (pow(0.65, ((double)PGA / 10))) - 110.35) / 20.4516 + 16; //From datasheet

                if (PGA <= 31) {
                    LMS_WriteParam(device, LMS7param(G_PGA_RBB), PGA);
                    LMS_WriteParam(device, LMS7param(RCC_CTL_PGA_RBB), rcc_ctl_pga_rbb);
                }

                ExtIOCallback(-1, extHw_Changed_ATT, 0, NULL);
                isCalibrated = NotCalibrated;
                UpdateDialog();
                return TRUE;
            }
        }
    }
    break;
    /* Command message */
    case WM_COMMAND:
    {
        switch (GET_WM_COMMAND_ID(wParam, lParam)) {

            /* Changed antenna */
        case IDC_COMBO_ANT:
        {
            if (GET_WM_COMMAND_CMD(wParam, lParam) == CBN_SELCHANGE) {

                int currentSel = ComboBox_GetCurSel(GET_WM_COMMAND_HWND(wParam, lParam));
                ant_select = ComboBox_GetItemData(GET_WM_COMMAND_HWND(wParam, lParam), currentSel);

                int64_t freq = -1;
                if (isRunning) {
                    freq = GetHWLO64();
                    StopHW();
                }

                if (LMS_SetAntenna(device, LMS_CH_RX, channel, ant_select) != LMS_SUCCESS) {
                    break;
                }

                PerformCalibration(false);

                if (freq != -1)
                    StartHW64(freq);

                UpdateDialog();

                return TRUE;
            }
        }
        break;
        /* Changed device */
        case IDC_COMBO_DEVICE:
        {
            if (GET_WM_COMMAND_CMD(wParam, lParam) == CBN_SELCHANGE) {
                if (currentDeviceIndex != ComboBox_GetCurSel(GET_WM_COMMAND_HWND(wParam, lParam))) {

                    int64_t freq = -1;
                    if (isRunning) {
                        freq = GetHWLO64();
                        StopHW();
                    }
                    if (LMS_Close(device) != LMS_SUCCESS)
                        break;

                    currentDeviceIndex = ComboBox_GetCurSel(GET_WM_COMMAND_HWND(wParam, lParam));

                    /* Default settings */
                    channel = 0;
                    sr_idx = 1;
                    ant_select = LMS_PATH_LNAH;

                    LNA = 10;
                    TIA = 3;
                    PGA = 16;

                    LPFenable = true;
                    LPFbandwidth = sampleRates[sr_idx];
                    CalibrationBandwidth = sampleRates[sr_idx];

                    InitializeLMS();

                    /* Remove all channel selections */
                    while (ComboBox_GetCount(GetDlgItem(hwndDlg, IDC_COMBO_CHAN)) != 0)
                        ComboBox_DeleteString(GetDlgItem(hwndDlg, IDC_COMBO_CHAN), 0);

                    /* Add channel selections */
                    for (int i = 0; i < numOfChannels; i++) {
                        std::string channels = "RX" + std::to_string(i + 1);
                        ComboBox_AddString(GetDlgItem(hwndDlg, IDC_COMBO_CHAN), channels.c_str());
                    }

                    while (ComboBox_GetCount(GetDlgItem(hwndDlg, IDC_COMBO_ANT)) != 0)
                        ComboBox_DeleteString(GetDlgItem(hwndDlg, IDC_COMBO_ANT), 0);

                    /* Add antenna choices */
                    if (DeviceInfo[currentDeviceIndex][0] == "LimeSDR Mini") {
                        ComboBox_AddString(GetDlgItem(hwndDlg, IDC_COMBO_ANT), "Auto");
                        ComboBox_SetItemData(GetDlgItem(hwndDlg, IDC_COMBO_ANT), 0, 255);
                        ComboBox_AddString(GetDlgItem(hwndDlg, IDC_COMBO_ANT), "LNA_H");
                        ComboBox_SetItemData(GetDlgItem(hwndDlg, IDC_COMBO_ANT), 1, 1);
                        ComboBox_AddString(GetDlgItem(hwndDlg, IDC_COMBO_ANT), "LNA_W");
                        ComboBox_SetItemData(GetDlgItem(hwndDlg, IDC_COMBO_ANT), 2, 3);
                    }
                    else if (DeviceInfo[currentDeviceIndex][0] == "LimeNET-Micro") {
                        ComboBox_AddString(GetDlgItem(hwndDlg, IDC_COMBO_ANT), "Auto");
                        ComboBox_SetItemData(GetDlgItem(hwndDlg, IDC_COMBO_ANT), 0, 255);
                        ComboBox_AddString(GetDlgItem(hwndDlg, IDC_COMBO_ANT), "LNA_H");
                        ComboBox_SetItemData(GetDlgItem(hwndDlg, IDC_COMBO_ANT), 1, 1);
                        ComboBox_AddString(GetDlgItem(hwndDlg, IDC_COMBO_ANT), "LNA_L");
                        ComboBox_SetItemData(GetDlgItem(hwndDlg, IDC_COMBO_ANT), 2, 2);
                    }
                    else {
                        ComboBox_AddString(GetDlgItem(hwndDlg, IDC_COMBO_ANT), "LNA_H");
                        ComboBox_SetItemData(GetDlgItem(hwndDlg, IDC_COMBO_ANT), 0, 1);
                        ComboBox_AddString(GetDlgItem(hwndDlg, IDC_COMBO_ANT), "LNA_L");
                        ComboBox_SetItemData(GetDlgItem(hwndDlg, IDC_COMBO_ANT), 1, 2);
                        ComboBox_AddString(GetDlgItem(hwndDlg, IDC_COMBO_ANT), "LNA_W");
                        ComboBox_SetItemData(GetDlgItem(hwndDlg, IDC_COMBO_ANT), 2, 3);
                    }

                    UpdateDialog();

                    /* Change last used device name */
                    strcpy(lastUsedDeviceName, DeviceInfo[currentDeviceIndex][0].c_str());

                    ExtIOCallback(-1, extHw_Changed_SampleRate, 0, NULL);
                    ExtIOCallback(-1, extHw_Changed_ATT, 0, NULL);

                    if (freq != -1)
                        StartHW64(freq);

                    return TRUE;
                }
            }
        }
        break;

        /* Changed channel */
        case IDC_COMBO_CHAN:
        {
            if (GET_WM_COMMAND_CMD(wParam, lParam) == CBN_SELCHANGE) {
                if (channel != ComboBox_GetCurSel(GET_WM_COMMAND_HWND(wParam, lParam))) {
                    int64_t freq = -1;
                    if (isRunning) {
                        freq = GetHWLO64();
                        StopHW();
                    }

                    if (LMS_EnableChannel(device, LMS_CH_RX, channel, false) != LMS_SUCCESS)
                        break;
                    if (LMS_EnableChannel(device, LMS_CH_TX, channel, false) != LMS_SUCCESS)
                        break;
                    channel = ComboBox_GetCurSel(GET_WM_COMMAND_HWND(wParam, lParam));

                    if (LMS_EnableChannel(device, LMS_CH_RX, channel, true) != LMS_SUCCESS)
                        break;
                    if (LMS_EnableChannel(device, LMS_CH_TX, channel, true) != LMS_SUCCESS)
                        break;
                    if (LMS_SetAntenna(device, LMS_CH_RX, channel, ant_select) != LMS_SUCCESS)
                        break;

                    if (LPFenable)
                        EnableLPF();
                    else
                        DisableLPF();

                    if (SetGain() != 0)
                        break;

                    PerformCalibration(false);

                    if (freq != -1)
                        StartHW64(freq);

                    UpdateDialog();

                    return TRUE;
                }
            }
        }
        break;

        /* Pressed Calibrate button */
        case IDC_BUTTON_CAl:
        {
            if (GET_WM_COMMAND_CMD(wParam, lParam) == BN_CLICKED) {

                int buffSize = GetWindowTextLength(GetDlgItem(hwndDlg, IDC_CAL_BW));
                char *textBuffer = new char[buffSize + 1];

                GetDlgItemText(hwndDlg, IDC_CAL_BW, textBuffer, buffSize + 1);

                CalibrationBandwidth = atof(textBuffer)*1e6;
                if (CalibrationBandwidth >= 2.5e6 && CalibrationBandwidth <= 120e6) {
                    PerformCalibration(true);

                    UpdateDialog();
                }
                else {
                    DbgPrintf("Frequency out of range, available range from 2.5 to 120 MHz");
                    CalibrationBandwidth = sampleRates[sr_idx];
                    if (CalibrationBandwidth < 2.5e6)
                        CalibrationBandwidth = 2.5e6;
                    UpdateDialog();
                }

                free(textBuffer);
                return TRUE;
            }
        }
        break;

        /* Pressed Set button*/
        case IDC_BUTTON_SET:
        {
            if (GET_WM_COMMAND_CMD(wParam, lParam) == BN_CLICKED) {

                int buffSize = GetWindowTextLength(GetDlgItem(hwndDlg, IDC_ALPF_BW));
                char *textBuffer = new char[buffSize + 1];

                GetDlgItemText(hwndDlg, IDC_ALPF_BW, textBuffer, buffSize + 1);

                LPFbandwidth = atof(textBuffer)*1e6;
                EnableLPF();
                PerformCalibration(false);
                UpdateDialog();

                free(textBuffer);
                return TRUE;
            }
        }
        break;
        /* LPF checkbox clicked */
        case IDC_CHECK_ALPF:
        {
            if (GET_WM_COMMAND_CMD(wParam, lParam) == BN_CLICKED) {
                if (IsDlgButtonChecked(hwndDlg, IDC_CHECK_ALPF) == BST_CHECKED) {
                    EnableLPF();
                    PerformCalibration(false);
                    UpdateDialog();
                }
                else if (IsDlgButtonChecked(hwndDlg, IDC_CHECK_ALPF) == BST_UNCHECKED) {
                    DisableLPF();
                    PerformCalibration(false);
                    UpdateDialog();
                }
            }
            return TRUE;
        }
        break;

        /* Pressed Reset button */
        case IDC_BUTTON_DEFAULT:
        {
            if (GET_WM_COMMAND_CMD(wParam, lParam) == BN_CLICKED) {
                int64_t freq = -1;
                if (isRunning) {
                    freq = GetHWLO64();
                    StopHW();
                }

                /* Default settings */
                LPFenable = true;
                channel = 0;
                sr_idx = 1;
                LPFbandwidth = sampleRates[sr_idx];
                CalibrationBandwidth = LPFbandwidth;
                ant_select = LMS_PATH_AUTO;

                LNA = 10;
                TIA = 3;
                PGA = 16;

                LMS_Close(device);
                InitializeLMS();

                ExtIOCallback(-1, extHw_Changed_SampleRate, 0, NULL);
                ExtIOCallback(-1, extHw_Changed_ATT, 0, NULL);

                if (freq != -1)
                    StartHW64(freq);

                UpdateDialog();

                return TRUE;
            }
        }
        break;
        }
    }
    break;
    /* Static text color message */
    case WM_CTLCOLORSTATIC:
    {
        /* Calibrated text color */
        if (GetDlgCtrlID((HWND)lParam) == IDC_TEXT_CALIBRATED) {
            HDC hdcStatic = (HDC)wParam;
            if (isCalibrated == Calibrated)
                SetTextColor(hdcStatic, RGB(24, 135, 0));
            else if (isCalibrated == NotCalibrated)
                SetTextColor(hdcStatic, RGB(0, 0, 0));
            else if (isCalibrated == CalibrationErr)
                SetTextColor(hdcStatic, RGB(255, 0, 0));

            SetBkColor((HDC)wParam, COLORREF(GetSysColor(COLOR_3DFACE)));
            return (INT_PTR)GetSysColorBrush(COLOR_3DFACE);
        }
    }
    break;
    /* Closed dialog window */
    case WM_CLOSE:
        ShowWindow(h_dialog, SW_HIDE);
        return TRUE;
        break;
        /* Destroy dialog window */
    case WM_DESTROY:
        ShowWindow(h_dialog, SW_HIDE);
        h_dialog = NULL;
        return TRUE;
        break;
    }
    return FALSE;
}
//---------------------------------------------------------------------------
extern "C"
bool __declspec(dllexport) __stdcall InitHW(char *name, char *model, int& type)
{
    /* Create debug console window */
#ifdef _MYDEBUG
    if (AllocConsole()) {
        FILE* f;
        freopen_s(&f, "CONOUT$", "wt", stdout);
        SetConsoleTitle(TEXT("Debug Console ExtIO_Lime " VERNUM));
        SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_RED);
    }
#endif
    log_handler = &error;
    LMS_RegisterLogHandler(log_handler);
    numOfDevices = LMS_GetDeviceList(deviceList);
    if (numOfDevices < 1) {
        DbgPrintf("No LMS device\n");
        return false;
    }
    else if (numOfDevices < currentDeviceIndex + 1)
        currentDeviceIndex = 0;

    /* Get device info */
    for (int i = 0; i < numOfDevices; i++) {
        std::string string = deviceList[i];

        /* Find serial */
        size_t find = string.find("serial=");
        DeviceInfo[i][1] = string.substr(find + 7);
        find = DeviceInfo[i][1].find(",");
        DeviceInfo[i][1] = DeviceInfo[i][1].substr(0, find);
        /* Remove trailing zeroes */
        while (DeviceInfo[i][1][0] == '0')
            DeviceInfo[i][1].erase(0, 1);

        /* Find name */
        find = string.find(",");
        DeviceInfo[i][0] = string.substr(0, find);
    }

    /* If device was changed, reset settings to default */
    if (strcmp(lastUsedDeviceName, DeviceInfo[currentDeviceIndex][0].c_str()) != 0) {
        LPFenable = true;
        sr_idx = 1;
        LPFbandwidth = sampleRates[sr_idx];
        CalibrationBandwidth = LPFbandwidth;
        channel = 0;
        if (DeviceInfo[currentDeviceIndex][0] == "LimeSDR-USB")
            ant_select = LMS_PATH_LNAH;
        else
            ant_select = LMS_PATH_AUTO;
        LNA = 10;
        TIA = 3;
        PGA = 16;
    }

    strcpy(lastUsedDeviceName, DeviceInfo[currentDeviceIndex][0].c_str());
    type = exthwUSBdata16;
    strcpy(name, HWNAME);
    strcpy(model, DeviceInfo[currentDeviceIndex][0].c_str());

    return true;
}
//---------------------------------------------------------------------------
extern "C"
bool EXTIO_API OpenHW(void)
{
    if (InitializeLMS() != 0)
        return false;

    h_dialog = CreateDialog((HINSTANCE)&__ImageBase, MAKEINTRESOURCE(ExtioDialog), NULL, (DLGPROC)MainDlgProc);
    ShowWindow(h_dialog, SW_HIDE);

    buffer = new (std::nothrow) int16_t[EXT_BLOCKLEN * 2];

    return true;
}

//---------------------------------------------------------------------------
extern "C"
int  EXTIO_API StartHW(long LOfreq)
{
    int64_t ret = StartHW64((int64_t)LOfreq);
    return (int)ret;
}
//---------------------------------------------------------------------------
extern "C"
int EXTIO_API StartHW64(int64_t LOfreq)
{
    SetHWLO64(LOfreq);

    streamId.channel = channel;
    streamId.fifoSize = 1024 * 128;
    streamId.throughputVsLatency = 1;
    streamId.isTx = false;
    streamId.dataFmt = lms_stream_t::LMS_FMT_I16;

    if (LMS_SetupStream(device, &streamId) != LMS_SUCCESS)
        return -1;

    if (LMS_StartStream(&streamId) != LMS_SUCCESS)
        return -1;

    isRunning = true;

    thread_handle = (HANDLE)_beginthread(RecvThread, 0, NULL);
    SetThreadPriority(thread_handle, THREAD_PRIORITY_TIME_CRITICAL);

    return EXT_BLOCKLEN;
}
//---------------------------------------------------------------------------
extern "C"
void EXTIO_API StopHW(void)
{
    if (isRunning) {
        isRunning = false;

        WaitForSingleObject(thread_handle, INFINITE);
        thread_handle = INVALID_HANDLE_VALUE;

        LMS_StopStream(&streamId);
        LMS_DestroyStream(device, &streamId);
    }
}
//---------------------------------------------------------------------------
extern "C"
void EXTIO_API CloseHW(void)
{
    LMS_Close(device);
    delete buffer;
    DestroyWindow(h_dialog);
}
//---------------------------------------------------------------------------
extern "C"
int  EXTIO_API SetHWLO(long LOfreq)
{
    int64_t ret = SetHWLO64((int64_t)LOfreq);
    return (ret & 0xFFFFFFFF);
}
//---------------------------------------------------------------------------
extern "C"
int64_t EXTIO_API SetHWLO64(int64_t LOfreq)
{
    int64_t ret = 0;
    float_type freq = float_type(LOfreq);
    if (LOfreq < 1e3) {
        freq = 1e3;
        ret = -1 * (1e3);
    }

    if (LOfreq > 3800e6) {
        freq = 3800e6;
        ret = 3800e6;
    }

    if (isRunning && freq < 30e6) {
        StopHW();
        StartHW64(freq);
    }
    else {
        if (CurrentLOfreq != LOfreq) {
            isCalibrated = NotCalibrated;
            LMS_SetLOFrequency(device, LMS_CH_RX, channel, freq);


            UpdateDialog();
            ExtIOCallback(-1, extHw_Changed_LO, 0, NULL);
        }
    }

    return ret;
}
//---------------------------------------------------------------------------
extern "C"
int  EXTIO_API GetStatus(void)
{
    return 0;
}
//---------------------------------------------------------------------------
extern "C"
void EXTIO_API SetCallback(pfnExtIOCallback funcptr)
{
    ExtIOCallback = funcptr;
    return;
}
//---------------------------------------------------------------------------
extern "C"
void EXTIO_API VersionInfo(const char * progname, int ver_major, int ver_minor)
{
    return;
}
//---------------------------------------------------------------------------
extern "C"
long EXTIO_API GetHWLO(void)
{
    int64_t	glLOfreq = GetHWLO64();
    return (long)(glLOfreq & 0xFFFFFFFF);
}
//---------------------------------------------------------------------------
extern "C"
int64_t EXTIO_API GetHWLO64(void)
{
    float_type freq;
    LMS_GetLOFrequency(device, LMS_CH_RX, channel, &freq);
    freq = ceil(freq);
    CurrentLOfreq = int64_t(freq);
    return CurrentLOfreq;
}
//---------------------------------------------------------------------------
extern "C"
long EXTIO_API GetHWSR(void)
{
    long SampleRate;

    if (device != nullptr) {
        float_type freq;

        LMS_GetSampleRate(device, LMS_CH_RX, channel, &freq, nullptr);

        SampleRate = round(freq);
    }
    return SampleRate;
}
//---------------------------------------------------------------------------
extern "C"
int EXTIO_API GetAttenuators(int atten_idx, float * attenuation)
{
    if (atten_idx < 74) {
        *attenuation = atten_idx;
        return 0;
    }
    return -1; // Finished
}
//---------------------------------------------------------------------------
extern "C"
int EXTIO_API GetActualAttIdx(void)
{
    unsigned int _gain = 0;

    if (device != nullptr) {
        if (LMS_GetGaindB(device, LMS_CH_RX, channel, &_gain) != LMS_SUCCESS) {
            return -1; // ERROR
        }
    }
    return _gain;
}
//---------------------------------------------------------------------------
extern "C"
int EXTIO_API SetAttenuator(int atten_idx)
{
    if (device != nullptr) {
        if (LMS_SetGaindB(device, LMS_CH_RX, channel, atten_idx) != LMS_SUCCESS) {
            return -1; // ERROR
        }
        isCalibrated = NotCalibrated;
        UpdateDialog();
    }
    return 0;
}
//---------------------------------------------------------------------------
extern "C"
int EXTIO_API ExtIoGetSrates(int srate_idx, double * samplerate)
{
    if (srate_idx < (sizeof(sampleRates) / sizeof(sampleRates[0]))) {
        *samplerate = sampleRates[srate_idx];
        return 0;
    }
    else {
        return -1; // Finished
    }
}
//---------------------------------------------------------------------------
extern "C"
int  EXTIO_API ExtIoGetActualSrateIdx(void)
{
    return sr_idx;
}
//---------------------------------------------------------------------------
extern "C"
int  EXTIO_API ExtIoSetSrate(int srate_idx)
{
    if (srate_idx >= 0 && srate_idx < (sizeof(sampleRates) / sizeof(sampleRates[0]))) {
        int64_t freq = 0;

        if (isRunning) {
            freq = GetHWLO64();
            StopHW();
        }

        if (LMS_SetSampleRate(device, sampleRates[srate_idx], oversample) != LMS_SUCCESS) {
            return -1;
        }

        CalibrationBandwidth = LPFbandwidth = sampleRates[srate_idx];
        if (CalibrationBandwidth < 2.5e6)
            CalibrationBandwidth = 2.5e6;

        if (LPFenable)
            EnableLPF();

        PerformCalibration(false);

        if (freq != 0) {
            StartHW64(freq);
        }

        sr_idx = srate_idx;
        ExtIOCallback(-1, extHw_Changed_SampleRate, 0, NULL);

        UpdateDialog();
        return 0;
    }

    return -1;	// ERROR
}
//---------------------------------------------------------------------------
extern "C"
int  EXTIO_API ExtIoGetSetting(int idx, char * description, char * value)
{

    switch (idx) {
    case 0:
        snprintf(description, 1024, "%s", "HW Name");
        strcpy_s(value, 1024, lastUsedDeviceName);
        return 0;
    case 1:
        snprintf(description, 1024, "%s", "Channel");
        snprintf(value, 1024, "%d", channel);
        return 0;
    case 2:
        snprintf(description, 1024, "%s", "Antenna");
        snprintf(value, 1024, "%d", ant_select);
        return 0;
    case 3:
        snprintf(description, 1024, "%s", "LPF bandwidth");
        snprintf(value, 1024, "%f", LPFbandwidth);
        return 0;
    case 4:
        snprintf(description, 1024, "%s", "LPF enable");
        snprintf(value, 1024, "%d", LPFenable);
        return 0;
    case 5:
        snprintf(description, 1024, "%s", "Srate index");
        snprintf(value, 1024, "%d", sr_idx);
        return 0;
    case 6:
        snprintf(description, 1024, "%s", "LNA gain");
        snprintf(value, 1024, "%d", LNA);
        return 0;
    case 7:
        snprintf(description, 1024, "%s", "TIA gain");
        snprintf(value, 1024, "%d", TIA);
        return 0;
    case 8:
        snprintf(description, 1024, "%s", "PGA gain");
        snprintf(value, 1024, "%d", PGA);
        return 0;
    case 9:
        snprintf(description, 1024, "%s", "Current Device index");
        snprintf(value, 1024, "%d", currentDeviceIndex);
        return 0;
    case 10:
        snprintf(description, 1024, "%s", "Calibration bandwidth");
        snprintf(value, 1024, "%f", CalibrationBandwidth);
        return 0;
    case 11:
        snprintf(description, 1024, "%s", "Current LO frequency");
        snprintf(value, 1024, "%lld", CurrentLOfreq);
        return 0;
    default: return -1; // ERROR
    }
    return -1; // ERROR
}
//---------------------------------------------------------------------------
extern "C"
void EXTIO_API ExtIoSetSetting(int idx, const char * value)
{
    int tempInt;
    int64_t temp64Int;
    float_type tempFloat;
    switch (idx) {
    case 0:
        strcpy(lastUsedDeviceName, value);
        return;
    case 1:
        tempInt = atoi(value);

        if (tempInt >= 0 && tempInt < 2) {
            channel = tempInt;
        }
        return;
    case 2:
        tempInt = atoi(value);

        if (tempInt >= 0 && tempInt < 4) {
            ant_select = tempInt;
        }
        return;
    case 3:
        tempFloat = atof(value);

        if (tempFloat > 1.4e6 && tempFloat <= 130e6) {
            LPFbandwidth = tempFloat;
        }
        return;
    case 4:
        tempInt = atoi(value);

        if (tempInt == 0 || tempInt == 1) {
            LPFenable = tempInt;
        }
        return;
    case 5:
        tempInt = atoi(value);

        if (tempInt >= 0 && tempInt < (sizeof(sampleRates) / sizeof(sampleRates[0]))) {
            sr_idx = tempInt;
        }
        return;
    case 6:
        tempInt = atoi(value);

        if (tempInt > 0 && tempInt < 16) {
            LNA = tempInt;
        }
        return;
    case 7:
        tempInt = atoi(value);

        if (tempInt > 0 && tempInt < 4) {
            TIA = tempInt;
        }
        return;
    case 8:
        tempInt = atoi(value);

        if (tempInt >= 0 && tempInt < 32) {
            PGA = tempInt;
        }
        return;
    case 9:
        currentDeviceIndex = atoi(value);
        return;
    case 10:
        tempFloat = atof(value);

        if (tempFloat >= 2.5e6 && tempFloat <= 120e6) {
            CalibrationBandwidth = tempFloat;
        }
        return;
    case 11:
        temp64Int = _atoi64(value);

        if (temp64Int > 1e3 && temp64Int <= 3800e6) {
            CurrentLOfreq = temp64Int;
        }
        return;
    default: return;
    }
}
//---------------------------------------------------------------------------
extern "C"
void EXTIO_API ShowGUI()
{
    ShowWindow(h_dialog, SW_SHOW);
    return;
}
//---------------------------------------------------------------------------
extern "C"
void EXTIO_API HideGUI()
{
    ShowWindow(h_dialog, SW_HIDE);
    return;
}
//---------------------------------------------------------------------------
extern "C"
void EXTIO_API SwitchGUI()
{
    if (IsWindowVisible(h_dialog))
        ShowWindow(h_dialog, SW_HIDE);
    else
        ShowWindow(h_dialog, SW_SHOW);
    return;
}
//---------------------------------------------------------------------------