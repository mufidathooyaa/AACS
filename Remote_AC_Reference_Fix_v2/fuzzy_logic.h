#ifndef FUZZY_LOGIC_H
#define FUZZY_LOGIC_H

#include <Arduino.h>

// ============================================================
// MESIN FUZZY LOGIC MAMDANI (TRANSLASI DARI PYTHON SCIKIT-FUZZY)
// ============================================================
float trapmf(float x, float a, float b, float c, float d) {
  // Endpoint a/d termasuk ke kurva. Ini penting untuk shoulder a==b atau
  // c==d; implementasi lama menghasilkan membership 0 tepat di 18/32 C dan
  // okupansi 1/8 sehingga tidak ada rule yang aktif.
  if (x < a || x > d) return 0.0;
  if (x >= a && x <= b) {
    if (a == b) return 1.0;
    return (x - a) / (b - a);
  }
  if (x >= b && x <= c) return 1.0;
  if (x >= c && x <= d) {
    if (c == d) return 1.0;
    return (d - x) / (d - c);
  }
  return 0.0;
}

float trimf(float x, float a, float b, float c) {
  // Puncak diperiksa lebih dulu agar segitiga batas a==b atau b==c
  // tetap bernilai 1 tepat di puncak (SP16 dan SP30).
  if (x == b) return 1.0;
  if (x <= a || x >= c) return 0.0;
  if (x > a && x < b) return (x - a) / (b - a);
  if (x > b && x < c) return (c - x) / (c - b);
  return 0.0;
}

int jalankan_inferensi_fuzzy(float suhuAktual, int okupansi) {
    if (okupansi == 0) return 30; 

    // Universe rule saat ini adalah 18..32 C dan 1..8 orang. Nilai valid di
    // luar universe dipetakan ke shoulder terdekat agar inferensi tidak jatuh
    // ke default 24 C justru pada kondisi paling dingin/panas/padat.
    const float suhuInput = constrain(suhuAktual, 18.0f, 32.0f);
    const float okupansiInput = constrain(static_cast<float>(okupansi), 1.0f, 8.0f);

    float s_dingin = trapmf(suhuInput, 18.0, 18.0, 20.0, 20.8);
    float s_sejuk  = trapmf(suhuInput, 20.0, 20.5, 22.8, 23.3);
    float s_nyaman = trapmf(suhuInput, 22.3, 22.8, 25.8, 26.3);
    float s_hangat = trapmf(suhuInput, 25.3, 25.8, 27.1, 27.8);
    float s_panas  = trapmf(suhuInput, 27.1, 27.8, 32.0, 32.0);

    float o_sedikit = trapmf(okupansiInput, 1.0, 1.0, 2.0, 4.0);
    float o_sedang  = trimf(okupansiInput, 2.0, 4.5, 7.0);
    float o_banyak  = trapmf(okupansiInput, 5.0, 7.0, 8.0, 8.0);

    float alpha[15];
    int rule_target[15] = {
        30, 28, 26,
        28, 26, 24,
        26, 24, 22,
        24, 22, 20,
        20, 18, 16
    };
    
    alpha[0] = min(s_dingin, o_sedikit); alpha[1] = min(s_dingin, o_sedang); alpha[2] = min(s_dingin, o_banyak);
    alpha[3] = min(s_sejuk, o_sedikit); alpha[4] = min(s_sejuk, o_sedang); alpha[5] = min(s_sejuk, o_banyak);
    alpha[6] = min(s_nyaman, o_sedikit); alpha[7] = min(s_nyaman, o_sedang); alpha[8] = min(s_nyaman, o_banyak);
    alpha[9] = min(s_hangat, o_sedikit); alpha[10] = min(s_hangat, o_sedang); alpha[11] = min(s_hangat, o_banyak);
    alpha[12] = min(s_panas, o_sedikit); alpha[13] = min(s_panas, o_sedang); alpha[14] = min(s_panas, o_banyak);

    float pembilang = 0.0;
    float penyebut = 0.0;
    
    for (int i = 0; i <= 140; i++) {
        float x = 16.0 + (i * 0.1);
        float agregasi_max = 0.0;
        for(int r = 0; r < 15; r++) {
            if (alpha[r] > 0) {
                float kurva = 0;
                int target = rule_target[r];
                // Delapan set output overlap dengan jarak puncak 2 C.
                // Segitiga batas: [16,16,18] dan [28,30,30].
                if (target == 16) kurva = trimf(x, 16.0, 16.0, 18.0);
                else if (target == 30) kurva = trimf(x, 28.0, 30.0, 30.0);
                else kurva = trimf(x, target - 2.0, target, target + 2.0);
                float implikasi = min(alpha[r], kurva);
                if (implikasi > agregasi_max) agregasi_max = implikasi;
            }
        }
        pembilang += agregasi_max * x;
        penyebut += agregasi_max;
    }
    
    if (penyebut == 0.0) return 24;
    
    float crisp_output = pembilang / penyebut;
    int setpoint_final = round(crisp_output);
    if (setpoint_final < 16) setpoint_final = 16;
    if (setpoint_final > 30) setpoint_final = 30;
    return setpoint_final;
}

#endif
