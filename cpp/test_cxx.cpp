/*
 * test_cxx.cpp — демонстрация того, что C++ работает в ядре.
 *
 * Глобальный объект Banner создаётся до kernel_main (через __init_array),
 * его конструктор вызывается из cxx_call_global_ctors().
 * Виртуальная функция greet() проверяет, что vtable собрана корректно.
 */

#include "../kernel/types.h"
#include <stdio.h>

class Greeter {
public:
    virtual ~Greeter() = default;
    virtual void greet() const = 0;
};

class Banner : public Greeter {
    const char* message_;
    int build_id_;
public:
    Banner(const char* m, int id) : message_(m), build_id_(id) {
        /* Конструктор выполняется до main — печатать сейчас рано,
           VGA ещё не инициализирован. Запомним и напечатаем потом. */
    }
    void greet() const override {
        printf("[cxx] %s (build #%d)\n", message_, build_id_);
    }
};

/* Глобальный объект — его конструктор попадёт в .init_array. */
static Banner g_banner("WebOS kernel says hi from C++", 1);

/* Экспортируем для main.c как C-функцию */
extern "C" void cxx_demo() {
    const Greeter& g = g_banner;
    g.greet();   /* проверяем, что виртуальная диспетчеризация работает */
}
