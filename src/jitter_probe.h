// Test aid (HLE_LOG_JITTER): how far the knight wobbles on screen while he runs. Each frame,
// where he is drawn (pixels from the view centre, with the view the frame is drawn with) is
// compared with where steady motion would put him, between the frames before and after it in
// time. Logged every 120 measured frames: the mean and worst wobble in pixels.
#pragma once
#include <math.h>
extern int deltaFrames;

static void jitter_frame(void)
{
    static float hx[3], hy[3], ht[3], t, sum, worst;
    static int n, m;
    t += deltaFrames;
    float vx, vz;
    character_get_velocity(&vx, &vz);
    if (vx * vx + vz * vz < 1.0f) { n = 0; return; }   // standing: nothing to compare
    float d[3] = { character.pos[0] - camPos.v[0], character.pos[1] + 10.0f - camPos.v[1], character.pos[2] - camPos.v[2] };
    float f[3] = { camTarget.v[0] - camPos.v[0], camTarget.v[1] - camPos.v[1], camTarget.v[2] - camPos.v[2] };
    float fl = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    f[0] /= fl; f[1] /= fl; f[2] /= fl;
    float r[3] = { -f[2], 0.0f, f[0] };                  // f x up, up = +y
    float rl = sqrtf(r[0] * r[0] + r[2] * r[2]); r[0] /= rl; r[2] /= rl;
    float u[3] = { r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0] };
    float z = d[0] * f[0] + d[1] * f[1] + d[2] * f[2];
    float px = 160.0f / tanf(FOV * 3.14159265f / 360.0f);
    hx[0] = hx[1]; hx[1] = hx[2]; hy[0] = hy[1]; hy[1] = hy[2]; ht[0] = ht[1]; ht[1] = ht[2];
    hx[2] = px * (d[0] * r[0] + d[2] * r[2]) / z;
    hy[2] = px * (d[0] * u[0] + d[1] * u[1] + d[2] * u[2]) / z;
    ht[2] = t;
    if (++n < 3) return;
    float a = (ht[1] - ht[0]) / (ht[2] - ht[0]);
    float ex = hx[1] - (hx[0] + (hx[2] - hx[0]) * a), ey = hy[1] - (hy[0] + (hy[2] - hy[0]) * a);
    float e = sqrtf(ex * ex + ey * ey);
    sum += e; if (e > worst) worst = e;
    if (++m == 120) {
        debugf("[jitter] knight wobbles %.2f px on average, worst %.2f px\n", (double)(sum / m), (double)worst);
        sum = worst = 0.0f; m = 0;
    }
}
