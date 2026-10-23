# База знаний ошибок, граблей и неудачных экспериментов (FAILURE LOG)
> **Назначение файла**: Полный реестр всех ошибочных решений, тупиковых гипотез и проверенных антипаттернов.
> **Правило разработчика**: ПЕРЕД любым изменением пинов, регистров или архитектуры ОБЯЗАТЕЛЬНО свериться с этим файлом!

---


---

### [Инцидент 2026-10-23 15:28:34] GyverMenu 1.1.4 breaking API changes
- **Симптом и ущерб**: Build break in src/main.cpp: ValueFloat signature mismatch, ValueStr missing, onCursor signature changed
- **Причина (Root Cause)**: GyverMenu 1.1.4 changed API compared to 1.0.2 (ValueFloat expects 4 args, ValueStr replaced, onCursor has 2 params)
- **Решение и правило**: Lock GyverMenu to 1.0.2 in platformio.ini (gyverlibs/GyverMenu @ 1.0.2)
