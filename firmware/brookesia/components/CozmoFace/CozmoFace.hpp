// Rosto do Cozmo do cozmo_ype (~/Documents/Arduino/cozmo_ype): olhos, boca e tontura ao chacoalhar.
// Usado pelo app Cozmo e pelo AIChats. Tudo roda num lv_timer, então só chame com a LVGL travada.
#pragma once

#include <array>
#include <cstdint>

#include "lvgl.h"

namespace esp_brookesia::apps {

class CozmoFace {
public:
    enum Humor { NEUTRO, FELIZ, TRISTE, BRAVO, SONO, CURIOSO, SURPRESO, PENSANDO, NHUMORES };

    ~CozmoFace() { destroy(); }
    // cy = centro vertical do rosto dentro do parent; escala 1 = OLED 128x64
    void create(lv_obj_t *parent, int cx, int cy, float escala);
    void destroy();  // apaga o timer; os objetos somem junto com o parent

    void setHumor(Humor h) { _humor = h; }
    void setEmocao(const char *emocao);  // emoções do XiaoZhi (happy, sad...)
    void setFalando(bool falando) { _falando = falando; }
    void tontura();                      // também dispara sozinho ao chacoalhar
    Humor humor() const { return _humor; }
    uint32_t tonturas() const { return _tonturas; }  // conta as vezes que ficou tonto

private:
    enum { W, H, R, DY, TT, TO, HR, DYR, MW, MH, MC, NP };
    static void quadro(lv_timer_t *t);
    void tick();
    bool chacoalhou(uint32_t agora);
    void olho(int i, float cx, float h, float dy);
    void boca(float dx, float dy, float fala);
    void estrelas(uint32_t agora, bool visiveis);

    lv_timer_t *_timer = nullptr;
    std::array<lv_obj_t *, 2> _olho = {}, _palpebra = {};
    lv_point_precise_t _pontos[2][2] = {};  // lv_line guarda o ponteiro
    lv_obj_t *_boca = nullptr, *_corte_boca = nullptr;
    std::array<lv_obj_t *, 5> _estrela = {};
    std::array<float, NP> _atual = {};
    int _cx = 233, _cy = 233;
    float _esc = 3;
    Humor _humor = NEUTRO;
    bool _falando = false;
    uint32_t _tonto_ate = 0, _inicio_piscada = 0, _duracao_piscada = 140, _proxima_piscada = 3000;
    uint32_t _picos[4] = {};  // instantes das últimas sacudidas fortes
    unsigned _npicos = 0;
    volatile uint32_t _tonturas = 0;
};

} // namespace esp_brookesia::apps
