#include <M5Unified.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>

namespace {

int counter = 0;

void draw_counter()
{
    auto& display = M5.Display;

    display.fillScreen(TFT_BLACK);
    display.setTextDatum(middle_center);
    display.setTextColor(TFT_WHITE, TFT_BLACK);
    display.setFont(&fonts::FreeSansBoldOblique24pt7b);
    display.setTextSize(2);

    char buffer[24];
    snprintf(buffer, sizeof(buffer), "%d", counter);
    display.drawString(buffer, display.width() / 2, display.height() / 2);

    display.setFont(&fonts::Font2);
    display.setTextSize(1);
    display.setTextColor(TFT_DARKGREY, TFT_BLACK);
    display.drawString("Tap screen to increment", display.width() / 2, display.height() - 22);
}

}  // namespace

extern "C" void app_main(void)
{
    M5.begin();

    M5.Display.setBrightness(180);
    M5.Display.setRotation(1);

    draw_counter();

    while (true) {
        M5.update();

        auto touch = M5.Touch.getDetail();
        if (touch.wasPressed()) {
            ++counter;
            draw_counter();
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
