// App "Cozmo": o rosto do cozmo_ype (CozmoFace) em tela cheia que conversa (CozmoVoz). Dorme até ouvir
// "Hey Cozmo" ou levar um toque; tocar enquanto ele fala faz ele parar. Molde do app Crosshair.
#include <cstring>
#include <memory>

#include "CozmoFace.hpp"
#include "CozmoVoz.hpp"
#include "esp_brookesia.hpp"
#include "esp_lib_utils.h"
#include "lvgl.h"
#include "systems/phone/esp_brookesia_phone_app.hpp"

LV_IMG_DECLARE(img_app_cozmo);

namespace esp_brookesia::apps {

namespace {

constexpr char APP_NAME[] = "Cozmo";

class Cozmo final : public systems::phone::App {
public:
    static Cozmo *requestInstance()
    {
        static Cozmo *instancia = new Cozmo();
        return instancia;
    }

protected:
    Cozmo() : App(APP_NAME, &img_app_cozmo, true, false, false) {}

    bool run() override
    {
        lv_area_t area = getVisualArea();
        int w = lv_area_get_width(&area), h = lv_area_get_height(&area);
        lv_obj_t *tela = lv_screen_active();
        lv_obj_remove_flag(tela, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(tela, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(tela, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(tela, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(tela, 0, 0);
        lv_obj_add_event_cb(tela, aoTocar, LV_EVENT_SHORT_CLICKED, this);
        lv_obj_add_event_cb(tela, aoAcariciar, LV_EVENT_LONG_PRESSED, this);
        _rosto.create(tela, w / 2, h / 2, 3.0f);
        _aviso = lv_label_create(tela);
        lv_obj_set_width(_aviso, w * 2 / 3);
        lv_obj_set_style_text_align(_aviso, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(_aviso, lv_color_hex(0x64748b), 0);
        lv_obj_align(_aviso, LV_ALIGN_BOTTOM_MID, 0, -50);
        _relogio = lv_timer_create(atualiza, 300, this);
        _voz.start(&_rosto);
        return true;
    }

    bool back() override { return notifyCoreClosed(); }

    bool pause() override
    {
        _voz.stop();  // solta o microfone para os outros apps
        return true;
    }

    bool resume() override
    {
        _voz.start(&_rosto);
        return true;
    }

    bool close() override
    {
        // a Brookesia apaga os objetos da tela do app; timers e áudio são nossos
        _voz.stop();
        _rosto.destroy();
        if (_relogio) {
            lv_timer_delete(_relogio);
            _relogio = nullptr;
        }
        return true;
    }

private:
    static void aoTocar(lv_event_t *e)
    {
        static_cast<Cozmo *>(lv_event_get_user_data(e))->_voz.toque();
    }

    static void aoAcariciar(lv_event_t *e)  // dedo parado na tela por ~0,4 s
    {
        static_cast<Cozmo *>(lv_event_get_user_data(e))->_voz.carinho();
    }

    static void atualiza(lv_timer_t *t)
    {
        auto *self = static_cast<Cozmo *>(lv_timer_get_user_data(t));
        const char *s = self->_voz.status();
        if (strcmp(lv_label_get_text(self->_aviso), s)) lv_label_set_text(self->_aviso, s);
    }

    CozmoFace _rosto;
    CozmoVoz _voz;
    lv_obj_t *_aviso = nullptr;
    lv_timer_t *_relogio = nullptr;
};

} // namespace

ESP_UTILS_REGISTER_PLUGIN_WITH_CONSTRUCTOR(
    systems::base::App, Cozmo, APP_NAME, []() {
        return std::shared_ptr<Cozmo>(Cozmo::requestInstance(), [](Cozmo *) {});
    }
)

} // namespace esp_brookesia::apps
