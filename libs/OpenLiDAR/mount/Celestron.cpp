#include "Celestron.h"

#include <math.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <cstring>
#include <termios.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/select.h> 

#include <map>
#include <string>

#define NULL_PTR(x) (x *)0
#define MAX_RESP_SIZE   20
#define TIMEOUT 5

/* Starsense specific constants */
#define ISNEXSTAR       0x11
#define ISSTARSENSE     0x13
#define MINSTSENSVER    float(1.18)

// device IDs
#define CELESTRON_DEV_RA  0x10
#define CELESTRON_DEV_DEC 0x11

/* Serial communication utilities */
typedef fd_set telfds;

static int writen(int _fd, const char* _ptr, int _nbytes) {
    int nleft, nwritten;
    nleft = _nbytes;
    while (nleft > 0) {
        nwritten = write(_fd, _ptr, nleft);
        if (nwritten <= 0)
            break;
        nleft -= nwritten;
        _ptr += nwritten;
    }
    return (_nbytes - nleft);
}

/*
 * Examines the read status of a file descriptor.
 * The timeout (sec, usec) specifies a maximum interval to
 * wait for data to be available in the descriptor.
 * To effect a poll, the timeout (sec, usec) should be 0.
 * Returns non-negative value on data available.
 * 0 indicates that the time limit referred by timeout expired.
 * On failure, it returns -1 and errno is set to indicate the
 * error.
 */
static int telstat(int _fd, int _sec, int _usec) {
    int ret;
    int width;
    struct timeval timeout;
    telfds readfds;

    memset((char *)&readfds, 0, sizeof(readfds));
    FD_SET(_fd, &readfds);
    width           = _fd + 1;
    timeout.tv_sec  = _sec;
    timeout.tv_usec = _usec;
    ret             = select(width, &readfds, NULL_PTR(telfds), NULL_PTR(telfds), &timeout);
    return (ret);
}

static int readn(int _fd, char *_ptr, int _nbytes, int _sec) {
    int status;
    int nleft, nread;
    nleft = _nbytes;
    while (nleft > 0) {
        status = telstat(_fd, _sec, 0);
        if (status <= 0)
            break;
        nread = read(_fd, _ptr, nleft);

        if (nread <= 0)
            break;
        nleft -= nread;
        _ptr += nread;
    }
    return (_nbytes - nleft);
}

// Send a command to the mount. Return the number of bytes received or 0 if
// case of error
int Celestron::send_command(    const char *_cmd, int _cmd_len, 
                                char *_resp, int _resp_len ) {
    int nbytes = _resp_len;

    if (m_fd) {
        /* Flush the input (read) buffer */
        tcflush(m_fd, TCIOFLUSH);

        writen(m_fd, _cmd, _cmd_len);
        nbytes = readn(m_fd, _resp, _resp_len, TIMEOUT);
    }

    if (nbytes != _resp_len)
        return 0;

    if (_resp_len == 0)
        return true;

    _resp[nbytes] = '\0';

    return nbytes;
}

// Send a 'passthrough command' to the mount. Return the number of bytes
// received or 0 in case of error
int Celestron::send_passthrough(  int _dest, int _cmd_id, 
                                const char* _payload, int _payload_len, 
                                char *_response, int _response_len) {
    char cmd[8] = {0};

    cmd[0] = 0x50;
    cmd[1] = (char)(_payload_len + 1);
    cmd[2] = (char)_dest;
    cmd[3] = (char)_cmd_id;
    cmd[7] = (char)_response_len;

    // payload_len must be <= 3 !
    memcpy(cmd + 4, _payload, _payload_len);

    return send_command(cmd, 8, _response, _response_len + 1);
}

Celestron::Celestron() :
    m_fd(0) {
}

Celestron::~Celestron() {
    disconnect();
}

bool Celestron::connect(const char* _port) {
    struct termios tty;

    fprintf(stderr, "Connecting to port: %s\n", _port);

    if (m_fd != 0)
        return false;

    /* Make the connection */

    m_fd = open(_port, O_RDWR);
    if (m_fd == -1)
        return false;

    tcgetattr(m_fd, &tty);
    cfsetospeed(&tty, (speed_t)B9600);
    cfsetispeed(&tty, (speed_t)B9600);
    tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;
    tty.c_iflag = IGNBRK;
    tty.c_lflag = 0;
    tty.c_oflag = 0;
    tty.c_cflag |= CLOCAL | CREAD;
    tty.c_cc[VMIN]  = 1;
    tty.c_cc[VTIME] = 5;
    tty.c_iflag &= ~(IXON | IXOFF | IXANY);
    tty.c_cflag &= ~(PARENB | PARODD);
    tcsetattr(m_fd, TCSANOW, &tty);

    m_connected = checkConnection();

    return m_connected;
}

void Celestron::disconnect() {
    if (m_connected)
        close(m_fd);
    m_connected = false;
}

bool Celestron::echo() {
    char response[MAX_RESP_SIZE];
    if (!send_command("Kx", 2, response, 2))
        return false;

    return !strcmp(response, "x#");
}

bool Celestron::checkConnection() {
    for (int i = 0; i < 2; i++) {
        if (echo())
            return true;

        usleep(50000);
    }

    return false;
}

// check if the mount is aligned using the mount J command
bool Celestron::checkAligned() {
    char response[MAX_RESP_SIZE];
    if (!send_command("J", 1, response, 2))
        return false;

    return response[0] == 0x01;
}


bool Celestron::hibernate() {
    char response[MAX_RESP_SIZE];
    return send_command("x#", 2, response, 1);
}

bool Celestron::wakeup() {
    char response[MAX_RESP_SIZE];
    return send_command("y#", 2, response, 1);
}

bool Celestron::printFirmware() {
    char version[8], model[16], RAVersion[8], DEVersion[8];
    bool isGem = false;

    printf("Getting controller version...\n");
    if (!getVersion(version, 8))
        return false;
    float controllerVersion = atof(version);

    printf("Getting controller variant...\n");
    char controllerVariant = ISNEXSTAR;
    getVariant(&controllerVariant);

    if (((controllerVariant == ISSTARSENSE) &&
          controllerVersion >= MINSTSENSVER) ||
         (controllerVersion >= 2.2) ) {
        printf("Getting controller model...\n");
        if (!getModel(model, 16, &isGem))
            return false;
    }

    printf("Getting RA firmware version...\n");
    if (!getDevFirmware(CELESTRON_DEV_RA, RAVersion, 8))
        return false;

    printf("Getting DEC firmware version...\n");
    if (!getDevFirmware(CELESTRON_DEV_DEC, DEVersion, 8))
        return false;

    printf("Firmware Info HC Ver %s model %s %s %s mount, HW Ver %s\n",
            version,
            model,
            controllerVariant == ISSTARSENSE ? "StarSense" : "NexStar",
            isGem ? "GEM" : "Fork",
            RAVersion);

    return true;
}

bool Celestron::getVersion(char* _version, int _size) {
    char response[MAX_RESP_SIZE];
    if (!send_command("V", 1, response, 3))
        return false;

    snprintf(_version, _size, "%d.%02d\n", static_cast<uint8_t>(response[0]), static_cast<uint8_t>(response[1]));

    printf("Controller version: %s\n", _version);
    return true;
}

bool Celestron::getVariant(char* _variant) {
    char response[MAX_RESP_SIZE];
    if (!send_command("v", 1, response, 2))
        return false;

    *_variant = static_cast<uint8_t>(response[0]);
    return true;
}

bool Celestron::getModel(char* _model, int _size, bool* _isGem) {
    // extended list of mounts
    std::map<int, std::string> models = {
        {1, "GPS Series"},
        {3, "i-Series"},
        {4, "i-Series SE"},
        {5, "CGE"},
        {6, "Advanced GT"},
        {7, "SLT"},
        {9, "CPC"},
        {10, "GT"},
        {11, "4/5 SE"},
        {12, "6/8 SE"},
        {13, "CGE Pro"},
        {14, "CGEM DX"},
        {15, "LCM"},
        {16, "Sky Prodigy"},
        {17, "CPC Deluxe"},
        {18, "GT 16"},
        {19, "StarSeeker"},
        {20, "AVX"},
        {21, "Cosmos"},
        {22, "Evolution"},
        {23, "CGX"},
        {24, "CGXL"},
        {25, "Astrofi"},
        {26, "SkyWatcher"},
    };

    char response[MAX_RESP_SIZE];
    if (!send_command("m", 1, response, 2))
        return false;

    int m = static_cast<uint8_t>(response[0]);
    if (models.find(m) != models.end()) {
        strncpy(_model, models[m].c_str(), _size);
        printf("Mount model: %s\n", _model);
    }
    else {
        strncpy(_model, "Unknown", _size);
        printf("Unrecognized model %s.\n", _model);
    }

    // use model# to detect the GEMs
    // Only Gem mounts can report the pier side pointing state
    switch(m) {
        case 5:     // CGE
        case 6:     // AS-GT
        case 13:    // CGE 2
        case 14:    // EQ6
        case 20:    // AVX
        case 0x17:  // CGX
        case 0x18:  // CGXL
            *_isGem = true;
            break;
        default:
            *_isGem = false;
    }

    return true;
}

bool Celestron::getDevFirmware(int _dev, char* _version, int _size) {
    char response[MAX_RESP_SIZE];
    int rlen = send_passthrough(_dev, 0xfe, nullptr, 0, response, 2);

    switch (rlen) {
        case 2:
            snprintf(_version, _size, "%01d.0\n", static_cast<uint8_t>(response[0]));
            break;
        case 3:
            snprintf(_version, _size, "%d.%02d\n", static_cast<uint8_t>(response[0]), static_cast<uint8_t>(response[1]));
            break;
        default:
            return false;
    }

    return true;
}


void getSexComponents(double value, int *d, int *m, int *s) {
    *d = (int32_t)fabs(value);
    *m = (int32_t)((fabs(value) - *d) * 60.0);
    *s = (int32_t)rint(((fabs(value) - *d) * 60.0 - *m) * 60.0);

    // Special case if seconds are >= 59.5 so it will be rounded by rint above
    // to 60
    if (*s == 60)
    {
        *s  = 0;
        *m += 1;
    }
    if (*m == 60)
    {
        *m  = 0;
        *d += 1;
    }

    if (value < 0)
        *d *= -1;
}

bool Celestron::setLocation(double _longitude, double _latitude) {
    printf("Setting location (%.3f,%.3f)\n", _longitude, _latitude);

    // Convert from INDI standard to regular east/west -180 to 180
    if (_longitude > 180)
        _longitude -= 360;

    int lat_d, lat_m, lat_s;
    int long_d, long_m, long_s;
    getSexComponents(_latitude, &lat_d, &lat_m, &lat_s);
    getSexComponents(_longitude, &long_d, &long_m, &long_s);

    char cmd[9];
    cmd[0] = 'W';
    cmd[1] = abs(lat_d);
    cmd[2] = lat_m;
    cmd[3] = lat_s;
    cmd[4] = lat_d > 0 ? 0 : 1;
    cmd[5] = abs(long_d);
    cmd[6] = long_m;
    cmd[7] = long_s;
    cmd[8] = long_d > 0 ? 0 : 1;

    char response[MAX_RESP_SIZE];
    return send_command(cmd, 9, response, 1);
}

// // there are newer time commands that have the utc offset in 15 minute increments
// bool Celestron::setDatetime(struct ln_date* _utc, double _utc_offset) {
//     struct ln_zonedate local_date;

//     // Celestron takes local time
//     ln_date_to_zonedate(utc, &local_date, utc_offset * 3600);

//     char cmd[9];
//     cmd[0] = 'H';
//     cmd[1] = local_date.hours;
//     cmd[2] = local_date.minutes;
//     cmd[3] = local_date.seconds;
//     cmd[4] = local_date.months;
//     cmd[5] = local_date.days;
//     cmd[6] = local_date.years - 2000;

//     if (utc_offset < 0)
//         cmd[7] = 256 - ((uint16_t)fabs(utc_offset));
//     else
//         cmd[7] = ((uint16_t)fabs(utc_offset));

//     // Always assume standard time
//     cmd[8] = 0;

//     set_sim_response("#");
//     return send_command(cmd, 9, response, 1, false, true);
// }

double pnex2dd(uint32_t value) {
    return 360.0 * ((double)value / 0x100000000);
}

void parseCoordsResponse(char *response, double *d1, double *d2){
    uint32_t d1_int = 0, d2_int = 0;

    sscanf(response, "%x,%x#", &d1_int, &d2_int);

    *d1 = pnex2dd(d1_int);
    *d2 = pnex2dd(d2_int);
}

double trimDecAngle(double angle) {
    angle = angle - 360*floor(angle/360);
    if (angle < 0)
        angle += 360.0;

    if ((angle > 90.) && (angle <= 270.))
        angle = 180. - angle;
    else if ((angle > 270.) && (angle <= 360.))
        angle = angle - 360.;

    return angle;
}

// Convert decimal degrees to NexStar angle (precise)
uint32_t dd2pnex(double angle) {
    angle = angle - 360*floor(angle/360);
    if (angle < 0)
        angle += 360.0;

    return (uint32_t)(angle * 0x100000000 / 360.0);
}


bool Celestron::slewRADec(double _ra, double _dec) {
    char cmd[20];
    sprintf(cmd, "r%08X,%08X", dd2pnex(_ra*15), dd2pnex(_dec));

    char response[MAX_RESP_SIZE];
    return send_command(cmd, strlen(cmd), response, 1);
}

bool Celestron::slewAzAlt(double _az, double _alt) {
    char cmd[20];
    sprintf(cmd, "b%08X,%08X", dd2pnex(_az), dd2pnex(_alt));

    char response[MAX_RESP_SIZE];
    return send_command(cmd, strlen(cmd), response, 1);
}

bool Celestron::isSlewing() {
    char response[MAX_RESP_SIZE];
    if (!send_command("L", 1, response, 2))
        return false;

    return response[0] != '0';
}

bool Celestron::gotoRADec(double _ra, double _dec) {
    slewRADec(_ra, _dec);

    // Wait until the mount stop slewing
    double ra, dec;
    while (isSlewing()) {
        usleep(1000);
        getRADec(&ra, &dec);
        // std::cout << "az:  " << ra << " alt: " << dec << std::endl;
    }

    return ((ra == _ra) && (dec == _dec));
}

bool Celestron::gotoAzAlt(double _az, double _alt) {
    slewAzAlt(_az, _alt);

    // Wait until the mount stop slewing
    double az, alt;
    while (isSlewing()) {
        usleep(1000);
        getAzAlt(&az, &alt);
        // std::cout << "az:  " << az << " alt: " << alt << std::endl;
    }

    return ((az == _az) && (alt == _alt));
}

bool Celestron::sync(double _ra, double _dec){
    char cmd[20];
    sprintf(cmd, "s%08X,%08X", dd2pnex(_ra*15), dd2pnex(_dec));

    char response[MAX_RESP_SIZE];
    return send_command(cmd, strlen(cmd), response, 1);
}


bool Celestron::getRADec(double* _ra, double* _dec) {
    char response[MAX_RESP_SIZE];
    if (!send_command("e", 1, response, 18))
        return false;
    
    parseCoordsResponse(response, _ra, _dec);
    *_ra /= 15.0;
    *_dec = trimDecAngle(*_dec);

    return true;
}

bool Celestron::getAzAlt(double* _az, double* _alt) {
    char response[MAX_RESP_SIZE];
    if (!send_command("z", 1, response, 18))
        return false;
    
    parseCoordsResponse(response, _az, _alt);

    return true;
}


bool Celestron::move(CELESTRON_DIRECTION _dir, CELESTRON_SLEW_RATE _rate) {
    int dev = (_dir == CELESTRON_N || _dir == CELESTRON_S) ? CELESTRON_DEV_DEC : CELESTRON_DEV_RA;
    int cmd_id = (_dir == CELESTRON_N || _dir == CELESTRON_W) ? 0x24 : 0x25;
    char payload[1];
    payload[0] = _rate + 1;

    char response[MAX_RESP_SIZE];
    return send_passthrough(dev, cmd_id, payload, 1, response, 0);
}

bool Celestron::stop(CELESTRON_DIRECTION _dir) {
    int dev = (_dir == CELESTRON_N || _dir == CELESTRON_S) ? CELESTRON_DEV_DEC : CELESTRON_DEV_RA;
    char payload[] = { 0 };

    char response[MAX_RESP_SIZE];
    return send_passthrough(dev, 0x24, payload, 1, response, 0);
}

bool Celestron::abort() {
    char response[MAX_RESP_SIZE];
    return send_command("M", 1, response, 1);
}


bool Celestron::getTrackMode(CELESTRON_TRACK_MODE *_mode) {
    char response[MAX_RESP_SIZE];
    if (!send_command("t", 1, response, 2))
        return false;

    *_mode = ((CELESTRON_TRACK_MODE)response[0]);
    return true;
}

bool Celestron::setTrackMode(CELESTRON_TRACK_MODE _mode) {
    char cmd[3];
    sprintf(cmd, "T%c", _mode);

    char response[MAX_RESP_SIZE];
    return send_command(cmd, 2, response, 1);
}


/*****************************************************************
    PulseGuide commands, experimental
******************************************************************/

/*****************************************************************
    Send a guiding pulse to the  mount in direction "dir".
    "rate" should be a signed 8-bit integer in the range (-100,100) that
    represents the pulse velocity in % of sidereal.
    "duration_csec" is an unsigned  8-bit integer (0,255) with  the pulse
    duration in centiseconds (i.e. 1/100 s  =  10ms).
    The max pulse duration is 2550 ms.
******************************************************************/
int Celestron::sendPulse(CELESTRON_DIRECTION _dir, signed char _rate, unsigned char _duration_csec) {
    int dev = (_dir == CELESTRON_N || _dir == CELESTRON_S) ? CELESTRON_DEV_DEC : CELESTRON_DEV_RA;
    char payload[2];
    payload[0] = (_dir == CELESTRON_N || _dir == CELESTRON_W) ? _rate : -_rate;
    payload[1] = _duration_csec;

    char response[MAX_RESP_SIZE];
    return send_passthrough(dev, 0x26, payload, 2, response, 0);
}

/*****************************************************************
    Send the guiding pulse status check command to the mount for the motor
    responsible for "dir". If  a pulse is being executed, "pulse_state" is set
    to 1, whereas if the pulse motion has been  completed it is set to 0.
    Return "false" if the status command fails, otherwise return "true".
******************************************************************/
int Celestron::getPulseStatus(CELESTRON_DIRECTION _dir, bool& _pulse_state) {
    int dev = (_dir == CELESTRON_N || _dir == CELESTRON_S) ? CELESTRON_DEV_DEC : CELESTRON_DEV_RA;
    char payload[2] = {0, 0};

    char response[MAX_RESP_SIZE];
    if (!send_passthrough(dev, 0x27, payload, 2, response, 1))
        return false;

    _pulse_state = (bool)response[0];
    return true;
}