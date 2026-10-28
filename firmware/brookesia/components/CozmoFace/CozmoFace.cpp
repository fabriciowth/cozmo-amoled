#include "CozmoFace.hpp"

#include <cmath>
#include <cstring>

#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "esp_random.h"
#include "qmi8658.h"

namespace esp_brookesia::apps {

namespace {

constexpr char TAG[] = "CozmoFace";
//                                 W   H   R  DY  TT  TO   HR  DYR  MW  MH  MC
constexpr float OLHAR[CozmoFace::NHUMORES][11] = {
    {34, 28, 10, 0, 0, 0, 1.00f, 0, 24, 4, 0},      // NEUTRO
    {34, 18, 12, -3, 0, 0, 1.00f, 0, 34, 13, 9},    // FELIZ    - sorriso
    {32, 22, 10, 5, 0, 11, 1.00f, 0, 28, 13, -9},   // TRISTE   - boca virada
    {34, 24, 8, 0, 12, 0, 1.00f, 0, 30, 8, -5},     // BRAVO
    {34, 4, 2, 7, 0, 0, 1.00f, 0, 14, 3, 0},        // SONO
    {34, 28, 10, -2, 0, 0, 0.65f, 6, 14, 9, 0},     // CURIOSO  - boquinha
    {38, 30, 14, -2, 0, 0, 1.00f, 0, 18, 17, 0},    // SURPRESO - boca aberta
    {28, 20, 9, -5, 0, 0, 0.85f, -2, 16, 4, 0},     // PENSANDO
};
constexpr float TONTO[11] = {30, 22, 11, 0, 0, 0, 1.00f, 0, 16, 10, 0};  // olhos rodando, boca "o"
constexpr float FACE_MAXH = 30;
constexpr uint32_t COR = 0x22d3ee, COR_ESTRELA = 0xfacc15;
constexpr uint32_t TONTO_MS = 3500;
// Chacoalhar: 4 trancos acima de ~1,3 g além da gravidade em até 1,5 s. Ajuste se disparar à toa.
constexpr float TRANCO_MG = 1300, VOLTA_MG = 600;
constexpr uint32_t JANELA_MS = 1500;

qmi8658_dev_t imu;
bool imu_ok = false, imu_tentou = false;

// Mesma configuração do Gravitysphere (8 g, 500 Hz), que divide o sensor com a gente.
void iniciaImu()
{
    if (imu_tentou) return;
    imu_tentou = true;
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    imu_ok = bus && qmi8658_init(&imu, bus, QMI8658_ADDRESS_HIGH) == ESP_OK &&  // fica em mg
             qmi8658_set_accel_odr(&imu, QMI8658_ACCEL_ODR_500HZ) == ESP_OK;
    if (!imu_ok) ESP_LOGW(TAG, "sem acelerometro: tontura so por chamada");
}

lv_obj_t *retangulo(lv_obj_t *pai, uint32_t cor)
{
    lv_obj_t *o = lv_obj_create(pai);
    lv_obj_remove_style_all(o);
    lv_obj_set_style_bg_color(o, lv_color_hex(cor), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

} // namespace

void CozmoFace::create(lv_obj_t *parent, int cx, int cy, float escala)
{
    destroy();
    _cx = cx;
    _cy = cy;
    _esc = escala;
    for (auto &o : _olho) o = retangulo(parent, COR);
    for (auto &p : _palpebra) {  // depois dos olhos, para ficar por cima
        p = lv_line_create(parent);
        lv_obj_set_style_line_color(p, lv_color_black(), 0);
        lv_obj_remove_flag(p, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
    }
    _boca = retangulo(parent, COR);
    _corte_boca = retangulo(parent, 0x000000);

    // estrela de 5 pontas em contorno; todas usam os mesmos pontos
    static lv_point_precise_t pontos[11];
    float r = 6 * _esc;
    for (int i = 0; i < 11; i++) {
        float a = -M_PI / 2 + i * M_PI / 5, raio = (i % 2) ? r * 0.45f : r;
        pontos[i] = {(lv_value_precise_t)(r + raio * cosf(a)), (lv_value_precise_t)(r + raio * sinf(a))};
    }
    for (auto &e : _estrela) {
        e = lv_line_create(parent);
        lv_line_set_points(e, pontos, 11);
        lv_obj_set_size(e, (int)(2 * r) + 1, (int)(2 * r) + 1);
        lv_obj_set_style_line_width(e, (int)fmaxf(2, _esc), 0);
        lv_obj_set_style_line_rounded(e, true, 0);
        lv_obj_set_style_line_color(e, lv_color_hex(COR_ESTRELA), 0);
        lv_obj_set_style_transform_pivot_x(e, (int)r, 0);
        lv_obj_set_style_transform_pivot_y(e, (int)r, 0);
        lv_obj_remove_flag(e, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(e, LV_OBJ_FLAG_HIDDEN);
    }
    std::memcpy(_atual.data(), OLHAR[NEUTRO], sizeof(OLHAR[NEUTRO]));
    _tonto_ate = 0;
    _npicos = 0;
    iniciaImu();
    _timer = lv_timer_create(quadro, 33, this);
}

void CozmoFace::destroy()
{
    if (_timer) {
        lv_timer_delete(_timer);
        _timer = nullptr;
    }
}

void CozmoFace::setEmocao(const char *emocao)
{
    static const struct { const char *nome; Humor h; } MAPA[] = {
        {"happy", FELIZ}, {"laughing", FELIZ}, {"funny", FELIZ}, {"loving", FELIZ}, {"kissy", FELIZ},
        {"delicious", FELIZ}, {"winking", FELIZ}, {"cool", FELIZ}, {"relaxed", FELIZ}, {"confident", FELIZ},
        {"silly", FELIZ}, {"sad", TRISTE}, {"crying", TRISTE}, {"angry", BRAVO}, {"sleepy", SONO},
        {"confused", CURIOSO}, {"embarrassed", CURIOSO}, {"surprised", SURPRESO}, {"shocked", SURPRESO},
        {"thinking", PENSANDO},
    };
    Humor h = NEUTRO;
    for (auto &e : MAPA) {
        if (emocao && strcmp(emocao, e.nome) == 0) h = e.h;
    }
    _humor = h;
}

void CozmoFace::tontura()
{
    uint32_t agora = lv_tick_get();
    if ((int32_t)(_tonto_ate - agora) <= 0) _tonturas = _tonturas + 1;  // chacoalhar mais não conta de novo
    _tonto_ate = agora + TONTO_MS;
}

bool CozmoFace::chacoalhou(uint32_t agora)
{
    float x, y, z;
    if (!imu_ok || qmi8658_read_accel(&imu, &x, &y, &z) != ESP_OK) return false;
    float desvio = fabsf(sqrtf(x * x + y * y + z * z) - 1000);
    bool forte = desvio > TRANCO_MG;
    // conta só a subida de cada tranco; espera o sensor sossegar antes do próximo
    static bool armado = true;
    if (!forte) {
        if (desvio < VOLTA_MG) armado = true;
        return false;
    }
    if (!armado) return false;
    armado = false;
    _picos[_npicos++ % 4] = agora;
    return _npicos >= 4 && agora - _picos[_npicos % 4] < JANELA_MS;
}

void CozmoFace::quadro(lv_timer_t *t)
{
    static_cast<CozmoFace *>(lv_timer_get_user_data(t))->tick();
}

// i = 0 olho esquerdo, 1 direito. cx já inclui o balanço horizontal.
void CozmoFace::olho(int i, float cx, float h, float dy)
{
    float w = _atual[W] * _esc;
    h = fmaxf(fminf(h, FACE_MAXH * _esc), 2 * _esc);
    float r = fminf(_atual[R] * _esc, fminf(w, h) / 2);
    float x = cx - w / 2, y = _cy - 11 * _esc + dy - h / 2;
    lv_obj_set_pos(_olho[i], (int)x, (int)y);
    lv_obj_set_size(_olho[i], (int)w, (int)h);
    lv_obj_set_style_radius(_olho[i], (int)r, 0);

    // Pálpebra: linha preta grossa na diagonal, com a borda de baixo no corte do canto de dentro (TT) ou de
    // fora (TO). Linha é barata; o retângulo girado de antes ia para uma camada e derrubava a tela a 6 fps.
    float corte = fmaxf(_atual[TT], _atual[TO]) * _esc;
    if (corte < 2) {
        lv_obj_add_flag(_palpebra[i], LV_OBJ_FLAG_HIDDEN);
        return;
    }
    bool desce_direita = (i == 0) == (_atual[TT] > _atual[TO]);
    float xb = desce_direita ? x + w : x, xa = desce_direita ? x : x + w;  // canto que desce / que fica
    float lx = xa - xb, ly = -corte, len = sqrtf(lx * lx + ly * ly);
    float ux = lx / len, uy = ly / len;
    float nx = lx > 0 ? ly / len : -ly / len, ny = lx > 0 ? -lx / len : lx / len;  // normal para cima
    float grossura = corte + 8, meio = grossura / 2, sobra = meio + 4;
    float p0x = xb - ux * sobra + nx * meio, p0y = y + corte - uy * sobra + ny * meio;
    float p1x = xa + ux * sobra + nx * meio, p1y = y + uy * sobra + ny * meio;
    float ox = fminf(p0x, p1x) - meio, oy = fminf(p0y, p1y) - meio;  // objeto só do tamanho da linha
    _pontos[i][0] = {(lv_value_precise_t)(p0x - ox), (lv_value_precise_t)(p0y - oy)};
    _pontos[i][1] = {(lv_value_precise_t)(p1x - ox), (lv_value_precise_t)(p1y - oy)};
    lv_obj_remove_flag(_palpebra[i], LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_line_width(_palpebra[i], (int)grossura, 0);
    lv_obj_set_pos(_palpebra[i], (int)ox, (int)oy);
    lv_line_set_points(_palpebra[i], _pontos[i], 2);
}

// Boca do cozmo_ype: retângulo arredondado recortado por outro preto deslocado = sorriso ou boca virada.
void CozmoFace::boca(float dx, float dy, float fala)
{
    float w = _atual[MW] * _esc, h = _atual[MH] * _esc, c = _atual[MC] * _esc;
    if (fala > 0.01f) {  // falando: abre no ritmo das sílabas e a curva quase some
        h += fala * 11 * _esc;
        c *= 0.35f;
    }
    h = fmaxf(h, 2 * _esc);
    float r = fminf(h, w) / 2;
    int x = (int)(_cx + dx - w / 2), y = (int)(_cy + 11 * _esc + dy - h / 2);
    lv_obj_set_pos(_boca, x, y);
    lv_obj_set_size(_boca, (int)w, (int)h);
    lv_obj_set_style_radius(_boca, (int)r, 0);
    if (fabsf(c) < 0.5f * _esc) {
        lv_obj_add_flag(_corte_boca, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(_corte_boca, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(_corte_boca, x - 2 * (int)_esc, y - (int)c);
    lv_obj_set_size(_corte_boca, (int)w + 4 * (int)_esc, (int)h);
    lv_obj_set_style_radius(_corte_boca, (int)r, 0);
}

// Estrelinhas girando numa elipse em volta da cabeça; as de trás ficam menores e mais apagadas.
void CozmoFace::estrelas(uint32_t agora, bool visiveis)
{
    for (size_t k = 0; k < _estrela.size(); k++) {
        lv_obj_t *e = _estrela[k];
        if (!visiveis) {
            lv_obj_add_flag(e, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        float a = agora / 260.0f + k * 2 * M_PI / _estrela.size();
        float frente = (sinf(a) + 1) / 2;  // 0 atrás, 1 na frente
        float r = 6 * _esc;
        lv_obj_remove_flag(e, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(e, (int)(_cx + cosf(a) * 44 * _esc - r), (int)(_cy - 30 * _esc + sinf(a) * 7 * _esc - r));
        lv_obj_set_style_transform_scale(e, (int)(256 * (0.6f + 0.5f * frente)), 0);
        lv_obj_set_style_transform_rotation(e, (int)(agora / 2) % 3600, 0);
        lv_obj_set_style_line_opa(e, (lv_opa_t)(110 + 145 * frente), 0);
    }
}

void CozmoFace::tick()
{
    uint32_t agora = lv_tick_get();
    if (chacoalhou(agora)) tontura();
    bool tonto = (int32_t)(_tonto_ate - agora) > 0;
    const float *alvo = tonto ? TONTO : OLHAR[_humor];
    for (int i = 0; i < NP; i++) _atual[i] += (alvo[i] - _atual[i]) * 0.18f;

    float piscada = 1.0f;
    if (agora - _inicio_piscada < _duracao_piscada) {
        piscada = 1.0f - sinf((float)(agora - _inicio_piscada) / _duracao_piscada * M_PI) * 0.92f;
    } else if ((int32_t)(agora - _proxima_piscada) > 0 && !tonto) {
        _inicio_piscada = agora;
        _duracao_piscada = 110 + esp_random() % 60;
        _proxima_piscada = agora + 2500 + esp_random() % 3500;
    }

    // pensando: o olhar vagueia mais rápido, como quem procura a resposta
    float vel = _humor == PENSANDO ? 600.0f : 2300.0f;
    float dx = sinf(agora / vel) * (_humor == PENSANDO ? 6.0f : 3.0f);
    float dy = sinf(agora / 1400.0f) * 2.0f + _atual[DY];
    float fala = 1.0f, silaba = 0;
    if (_falando && _humor != SONO && !tonto) {  // duas senoides fora de fase imitam as sílabas
        silaba = fabsf(sinf(agora / 75.0f)) * (0.6f + 0.4f * sinf(agora / 210.0f));
        fala = 0.80f + 0.20f * silaba;
        dy += silaba * 2.5f;
        dx += sinf(agora / 520.0f) * 2.0f;
    }

    float h = _atual[H] * _esc * piscada * fala;
    float esq_x = 0, esq_y = 0, dir_x = 0, dir_y = 0, boca_x = 0;
    if (tonto) {  // cada olho roda num sentido e a cabeça balança
        float a = agora / 110.0f;
        esq_x = cosf(a) * 4;
        esq_y = sinf(a) * 4;
        dir_x = cosf(-a + 1.5f) * 4;
        dir_y = sinf(-a + 1.5f) * 4;
        dx = sinf(agora / 180.0f) * 5;
        boca_x = sinf(agora / 140.0f) * 3;
    }
    olho(0, _cx + (-22 + dx + esq_x) * _esc, h, (dy + esq_y) * _esc);
    olho(1, _cx + (22 + dx + dir_x) * _esc, h * _atual[HR], (dy + _atual[DYR] + dir_y) * _esc);
    boca((dx * 0.6f + boca_x) * _esc, 0, silaba);
    estrelas(agora, tonto);
}

} // namespace esp_brookesia::apps
