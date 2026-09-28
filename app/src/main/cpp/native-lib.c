#include <jni.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <android/log.h>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "Cemu", __VA_ARGS__)

#define CELL 0.5
#define HASH_BITS 18
#define HASH_SIZE (1 << HASH_BITS)
#define HASH_MASK (HASH_SIZE - 1)
#define MAX_HAND_POINTS 50000
#define MAX_TRACK_POINTS 200000
#define R_EARTH 6378137.0
#define PI 3.14159265358979323846

/* ================= 空间哈希网格 ================= */
static int *g_keys = NULL;
static unsigned char *g_used = NULL;
static int g_count = 0;

static inline unsigned int hash_key(int key) {
    unsigned int h = (unsigned int)key;
    h ^= h >> 16; h *= 0x85ebca6b; h ^= h >> 13;
    h *= 0xc2b2ae35; h ^= h >> 16;
    return h & HASH_MASK;
}

static void grid_insert(int i, int j) {
    int key = (i << 16) ^ (j & 0xFFFF);
    unsigned int idx = hash_key(key);
    int probe = 0;
    while (g_used[idx]) {
        if (g_keys[idx] == key) return;
        idx = (idx + 1) & HASH_MASK;
        if (++probe > 2000) return;
    }
    g_used[idx] = 1; g_keys[idx] = key; g_count++;
}

static void rasterize(double x0, double y0, double x1, double y1, double hw) {
    double dx = x1 - x0, dy = y1 - y0;
    double len = sqrt(dx*dx + dy*dy);
    if (len < 0.01 || len > 200.0) return;
    double minX = (x0<x1?x0:x1) - hw, maxX = (x0>x1?x0:x1) + hw;
    double minY = (y0<y1?y0:y1) - hw, maxY = (y0>y1?y0:y1) + hw;
    int i0 = (int)floor(minX/CELL), i1 = (int)floor(maxX/CELL);
    int j0 = (int)floor(minY/CELL), j1 = (int)floor(maxY/CELL);
    double ux = dx/len, uy = dy/len, hw2 = hw*hw;
    for (int i = i0; i <= i1; i++) {
        double cx = (i + 0.5) * CELL;
        for (int j = j0; j <= j1; j++) {
            double cy = (j + 0.5) * CELL;
            double t = (cx-x0)*ux + (cy-y0)*uy;
            if (t < 0) t = 0; else if (t > len) t = len;
            double qx = x0+ux*t, qy = y0+uy*t;
            double ddx = cx-qx, ddy = cy-qy;
            if (ddx*ddx + ddy*ddy <= hw2) grid_insert(i, j);
        }
    }
}

/* ================= 一维卡尔曼 ================= */
typedef struct { double x, v, p00, p02, p22; } KF1D;

static KF1D g_kfx, g_kfy;
static long long g_last_t = 0;
static int g_kf_init = 0;

static void kf1d_init(KF1D *kf, double x, double acc) {
    kf->x = x; kf->v = 0;
    kf->p00 = acc*acc; kf->p02 = 0; kf->p22 = 4.0;
}

static double kf1d_update(KF1D *kf, double m, double acc, double dt, double maxV) {
    double q = 1.5;
    double qdt3 = q*dt*dt*dt/3.0;
    double qdt2 = q*dt*dt/2.0;
    double qdt  = q*dt;
    double px = kf->x + kf->v * dt;
    double p00p = kf->p00 + 2*dt*kf->p02 + dt*dt*kf->p22 + qdt3;
    double p02p = kf->p02 + dt*kf->p22 + qdt2;
    double p22p = kf->p22 + qdt;
    double R = acc*acc;
    double K0 = p00p/(p00p+R);
    double K1 = p02p/(p00p+R);
    double inn = m - px;
    kf->x = px + K0*inn;
    kf->v = kf->v + K1*inn/dt;
    if (kf->v > maxV) kf->v = maxV;
    if (kf->v < -maxV) kf->v = -maxV;
    kf->p00 = (1-K0)*p00p;
    kf->p02 = (1-K0)*p02p;
    kf->p22 = p22p - K1*p02p;
    return kf->x;
}

/* ================= 状态 ================= */
static double g_origin_lat = 0, g_origin_lon = 0;
static int g_has_origin = 0;
static double g_last_x = 0, g_last_y = 0;
static int g_has_last = 0;
static double g_total_dist = 0;
static int g_point_count = 0;

static double g_hand_lat[MAX_HAND_POINTS];
static double g_hand_lon[MAX_HAND_POINTS];
static int g_hand_count = 0;

static double g_track_lat[MAX_TRACK_POINTS];
static double g_track_lon[MAX_TRACK_POINTS];
static int g_track_count = 0;

/* ================= 主更新 ================= */
static int update_location(double lat, double lon, double acc, long long now_ms,
                           double car_width, int is_car) {
    if (!g_has_origin) {
        g_origin_lat = lat;
        g_origin_lon = lon;
        g_has_origin = 1;
    }
    double lat0 = g_origin_lat * PI / 180.0;
    double mx = R_EARTH * (lon - g_origin_lon) * PI / 180.0 * cos(lat0);
    double my = R_EARTH * (lat - g_origin_lat) * PI / 180.0;
    double max_speed = is_car ? 20.0 : 5.0;

    if (!g_kf_init) {
        kf1d_init(&g_kfx, mx, acc);
        kf1d_init(&g_kfy, my, acc);
        g_last_t = now_ms;
        g_kf_init = 1;
        g_last_x = mx; g_last_y = my; g_has_last = 1;
        g_point_count = 1;
        if (g_hand_count < MAX_HAND_POINTS) {
            g_hand_lat[0] = lat; g_hand_lon[0] = lon; g_hand_count = 1;
        }
        if (g_track_count < MAX_TRACK_POINTS) {
            g_track_lat[0] = lat; g_track_lon[0] = lon; g_track_count = 1;
        }
        return 1;
    }

    double dt = (now_ms - g_last_t) / 1000.0;
    if (dt < 0.05) dt = 0.05;
    if (dt > 2.0) dt = 2.0;

    double px = g_kfx.x + g_kfx.v * dt;
    double py = g_kfy.x + g_kfy.v * dt;
    double dx = mx - px, dy = my - py;
    double err = sqrt(dx*dx + dy*dy);
    double th = 3.0 * acc;
    if (th < 8.0) th = 8.0;
    if (err > th) { g_last_t = now_ms; return 0; }

    double ox = kf1d_update(&g_kfx, mx, acc, dt, max_speed);
    double oy = kf1d_update(&g_kfy, my, acc, dt, max_speed);
    g_last_t = now_ms;

    double seg = sqrt((ox-g_last_x)*(ox-g_last_x) + (oy-g_last_y)*(oy-g_last_y));
    if (seg < 0.25) return 0;
    if (seg > 200.0) return 0;

    if (is_car) rasterize(g_last_x, g_last_y, ox, oy, car_width / 2.0);
    g_total_dist += seg;
    g_last_x = ox; g_last_y = oy;

    if (g_hand_count < MAX_HAND_POINTS) {
        g_hand_lat[g_hand_count] = lat;
        g_hand_lon[g_hand_count] = lon;
        g_hand_count++;
    }
    if (g_track_count < MAX_TRACK_POINTS) {
        g_track_lat[g_track_count] = lat;
        g_track_lon[g_track_count] = lon;
        g_track_count++;
    }
    g_point_count++;
    return 1;
}

/* ================= JNI ================= */
JNIEXPORT void JNICALL
Java_com_cemu_NativeBridge_nativeInit(JNIEnv *env, jclass cls) {
    if (!g_keys) g_keys = (int*)calloc(HASH_SIZE, sizeof(int));
    if (!g_used) g_used = (unsigned char*)calloc(HASH_SIZE, sizeof(unsigned char));
    LOGI("nativeInit");
}

JNIEXPORT void JNICALL
Java_com_cemu_NativeBridge_nativeReset(JNIEnv *env, jclass cls) {
    if (g_used) memset(g_used, 0, HASH_SIZE);
    g_count = 0; g_kf_init = 0; g_has_last = 0; g_has_origin = 0;
    g_total_dist = 0; g_point_count = 0; g_hand_count = 0; g_track_count = 0;
    g_last_x = g_last_y = 0; g_last_t = 0;
    LOGI("nativeReset");
}

JNIEXPORT jint JNICALL
Java_com_cemu_NativeBridge_nativeOnLocation(
    JNIEnv *env, jclass cls,
    jdouble lat, jdouble lon, jdouble acc, jlong now_ms,
    jdouble car_width, jint is_car) {
    return update_location(lat, lon, acc, now_ms, car_width, is_car);
}

JNIEXPORT jdouble JNICALL
Java_com_cemu_NativeBridge_nativeGetArea(JNIEnv *env, jclass cls, jint is_car) {
    if (is_car) return (double)g_count * CELL * CELL;
    if (g_hand_count < 3) return 0.0;
    double lat0 = g_hand_lat[0] * PI / 180.0;
    double s = 0.0;
    for (int i = 0; i < g_hand_count; i++) {
        int j = (i+1) % g_hand_count;
        double x1 = R_EARTH*(g_hand_lon[i]-g_hand_lon[0])*PI/180.0*cos(lat0);
        double y1 = R_EARTH*(g_hand_lat[i]-g_hand_lat[0])*PI/180.0;
        double x2 = R_EARTH*(g_hand_lon[j]-g_hand_lon[0])*PI/180.0*cos(lat0);
        double y2 = R_EARTH*(g_hand_lat[j]-g_hand_lat[0])*PI/180.0;
        s += x1*y2 - x2*y1;
    }
    return fabs(s/2.0);
}

JNIEXPORT jdouble JNICALL
Java_com_cemu_NativeBridge_nativeGetDistance(JNIEnv *env, jclass cls) {
    return g_total_dist;
}

JNIEXPORT jint JNICALL
Java_com_cemu_NativeBridge_nativeGetCount(JNIEnv *env, jclass cls) {
    return g_point_count;
}

JNIEXPORT jstring JNICALL
Java_com_cemu_NativeBridge_nativeGetTrackJson(JNIEnv *env, jclass cls) {
    int n = g_track_count;
    if (n > 3000) n = 3000;
    char *buf = (char*)malloc(n * 32 + 16);
    if (!buf) return (*env)->NewStringUTF(env, "[]");
    int pos = 0;
    buf[pos++] = '[';
    int step = g_track_count / n;
    if (step < 1) step = 1;
    int first = 1;
    for (int i = 0; i < g_track_count && pos < n*32; i += step) {
        pos += sprintf(buf+pos, "%s[%.7f,%.7f]", first?"":",",
                       g_track_lat[i], g_track_lon[i]);
        first = 0;
    }
    buf[pos++] = ']'; buf[pos] = 0;
    jstring ret = (*env)->NewStringUTF(env, buf);
    free(buf);
    return ret;
}

/* ================= NMEA 解析（RTK 用） ================= */
JNIEXPORT jdoubleArray JNICALL
Java_com_cemu_NativeBridge_nativeParseNmea(JNIEnv *env, jclass cls, jstring line) {
    const char *s = (*env)->GetStringUTFChars(env, line, NULL);
    if (!s) return NULL;

    double lat = 0, lon = 0, alt = 0;
    int quality = 0, ok = 0;

    if (strstr(s, "$GNGGA") || strstr(s, "$GPGGA")) {
        char *p = strchr(s, ',');
        if (p) {
            for (int f = 1; f <= 5 && p; f++) {
                p++;
                char *comma = strchr(p, ',');
                if (!comma) break;
                int len = comma - p;
                char tmp[32] = {0};
                if (len > 31) len = 31;
                memcpy(tmp, p, len);
                if (f == 2) {
                    double v = atof(tmp);
                    int d = (int)(v/100);
                    lat = d + (v - d*100)/60.0;
                } else if (f == 3) {
                    if (tmp[0] == 'S') lat = -lat;
                } else if (f == 4) {
                    double v = atof(tmp);
                    int d = (int)(v/100);
                    lon = d + (v - d*100)/60.0;
                } else if (f == 5) {
                    if (tmp[0] == 'W') lon = -lon;
                }
                p = comma;
            }
            char *q = strstr(s, ",,");
            if (q) {
                q += 2;
                char *c2 = strchr(q, ',');
                if (c2) {
                    char t2[8] = {0};
                    int l2 = c2 - q;
                    if (l2 > 7) l2 = 7;
                    memcpy(t2, q, l2);
                    quality = atoi(t2);
                }
            }
            ok = 1;
        }
    }

    (*env)->ReleaseStringUTFChars(env, line, s);
    if (!ok) return NULL;

    double acc = (quality >= 4) ? 0.02 :
                 (quality == 3) ? 0.5 :
                 (quality == 2) ? 2.0 :
                 (quality == 1) ? 5.0 : 30.0;

    jdoubleArray arr = (*env)->NewDoubleArray(env, 4);
    double buf[4] = { lat, lon, alt, acc };
    (*env)->SetDoubleArrayRegion(env, arr, 0, 4, buf);
    return arr;
}
