# {{ ydb-short-name }} Быстрый старт

В этом руководстве вы установите одноузловой локальный [кластер {{ ydb-short-name }}](../concepts/databases.md#cluster) и выполните простые запросы к вашей [базе данных](../concepts/databases.md#database).

Обычно {{ ydb-short-name }} хранит данные напрямую на нескольких дисковых устройствах SSD/NVMe или HDD без использования файловой системы. Однако для простоты данное руководство эмулирует диски в оперативной памяти или с использованием файла в обычной файловой системе. Таким образом, эта конфигурация не подходит для использования в промышленном окружении и даже для проведения тестов производительности. Ознакомьтесь с [документацией по управлению кластерами](../cluster/index.md) для запуска {{ ydb-short-name }} в промышленном окружении.

## Установите и запустите {{ ydb-short-name }} {#install}

{% list tabs %}

- Linux x86_64

   {% note info %}

   Рекомендуемая среда для запуска {{ ydb-short-name }} - это x86_64 Linux. Если у вас нет доступа к такой системе, можете переключиться на инструкции во вкладке "Docker".

   {% endnote %}

   1. Создайте каталог для тестирования {{ ydb-short-name }} и используйте его в качестве текущего рабочего каталога:

      ```bash
      mkdir ~/ydbd && cd ~/ydbd
      ```

   2. Загрузите и запустите скрипт установки:

      ```bash
      curl {{ ydb-binaries-url }}/local_scripts/install.sh | bash
      ```

      Это действие загрузит и распакует архив с исполняемым файлом `ydbd`, библиотеками, файлами конфигурации и скриптами, необходимыми для запуска и остановки локального кластера.

      Скрипт выполняется с текущими привилегиями пользователя (обратите внимание на отсутствие `sudo`). Таким образом, он не может сделать многого в системе. Чтобы проверить какие именно команды он выполняет — откройте этот же URL в вашем браузере.

   3. Запустите кластер в одном из следующих режимов хранения данных:

      * Данные в оперативной памяти:

         ```bash
         ./start.sh ram
         ```

         В этом случае все данные хранятся только в оперативной памяти, и они будут потеряны при остановке кластера.

      * Данные в файле на диске:

         ```bash
         ./start.sh disk
         ```

         При первом запуске скрипта будет создан файл `ydb.data` размером 80 ГБ в рабочем каталоге. Убедитесь, что у вас достаточно свободного места на диске для его создания. Этот файл будет использоваться для эмуляции дискового устройства, которое использовалось бы в промышленном окружении.

      Результат:

      ```text
      Starting storage process...
      Initializing storage ...
      Registering database ...
      Starting database process...

      Database started. Connection options for YDB CLI:

      -e grpc://localhost:2136 -d /Root/test
      ```

- Docker

   1. Создайте каталог для тестирования {{ ydb-short-name }} и используйте его в качестве текущего рабочего каталога:

      ```bash
      mkdir ~/ydbd && cd ~/ydbd
      ```
   2. Загрузите текущую версию Docker образа:

      ```bash
      docker pull {{ ydb_local_docker_image }}:{{ ydb_local_docker_image_tag }}
      ```

   3. Запустите Docker контейнер:

      ```bash
      docker run -d --rm --name ydb-local -h localhost \
        -p 2135:2135 -p 2136:2136 -p 8765:8765 \
        -v $(pwd)/ydb_certs:/ydb_certs -v $(pwd)/ydb_data:/ydb_data \
        -e GRPC_TLS_PORT=2135 -e GRPC_PORT=2136 -e MON_PORT=8765 \
        {{ ydb_local_docker_image}}:{{ ydb_local_docker_image_tag }}
      ```

      Если контейнер успешно запустился, вы увидите его идентификатор. Контейнеру может потребоваться несколько минут для инициализации. База данных будет недоступна до окончания инициализации.

{% endlist %}


## Запустите первый запрос "Hello, world!"

Самый простой способ выполнить свой первый запрос к {{ ydb-short-name }} - это встроенный веб-интерфейс. Он запускается по умолчанию на порту 8765 сервера {{ ydb-short-name }}, поэтому, если вы запустили его локально, вам нужно открыть [localhost:8765](http://localhost:8765) в вашем веб-браузере. Если нет, замените `localhost` на имя хоста вашего сервера в этом URL, либо используйте `ssh -L 8765:localhost:8765 my-server-hostname-or-ip.example.com` для настройки проброса порта и все равно откройте [localhost:8765](http://localhost:8765). Вы увидите страницу подобного вида:

![Стартовая страница веб-интерфейса](_assets/web-ui-home.png)

{{ ydb-short-name }} спроектирован как многопользовательская система с возможностью одновременной работы тысяч пользователей с одним кластером. Как следствие, большинство логических сущностей внутри кластера {{ ydb-short-name }} находятся в гибкой иерархической структуре, больше похожей на виртуальную файловую систему Unix, чем на схему с фиксированной глубиной, с которой вы, возможно, знакомы из других систем управления базами данных. Как видите, первый уровень иерархии состоит из баз данных, работающих в одном процессе {{ ydb-short-name }}, которые могут принадлежать разным пользователям. `/Root` предназначена для системных целей, а `/Root/test` или `/local` (имя зависит от выбранного способа установки) - это «игровая площадка», созданная в процессе установки на предыдущем шаге. Давайте нажмём на последнюю, `/Root/test` или `/local`, затем введем наш первый запрос и нажмем кнопку запуска:

```sql
SELECT "Hello, world!"u;
```

Запрос возвращает приветствие, как и задумано:

![SELECT "Hello, world!"u;](_assets/select-hello-world.png)

{% note info %}

Заметили странный суффикс `u`? {{ ydb-short-name }} и её язык запросов YQL являются строго типизированными. Обычные строки в {{ ydb-short-name }} могут содержать любые двоичные данные, в то время как этот суффикс указывает, что этот литерал имеет тип данных `Utf8`, который может содержать только валидные последовательности [UTF-8](https://en.wikipedia.org/wiki/UTF-8). [Узнайте больше](../yql/reference/types/index.md) о системе типов {{ ydb-short-name }}.

{% endnote %}

Второй по простоте способ выполнения SQL-запроса с использованием {{ ydb-short-name }} - это [интерфейс командной строки (CLI)](../reference/ydb-cli/index.md). Большинство реальных приложений же, скорее всего, будут работать с {{ ydb-short-name }} через один из доступных [наборов инструментов для разработчиков программного обеспечения (SDK)](../reference/ydb-sdk/index.md). Если вы чувствуете себя уверенно — можете продолжить прохождение данного руководства с помощью одного из этих методов вместо веб-интерфейса.

## Создайте свою первую таблицу

Основная цель существования систем управления базами данных - сохранение данных для последующего извлечения. Как система, базирующаяся на SQL, основной абстракцией {{ ydb-short-name }} для хранения данных является таблица. Чтобы создать нашу первую таблицу, выполните следующий запрос:

```sql
CREATE TABLE example
(
    key UInt64,
    value String,
    PRIMARY KEY (key)
);
```

Как видите, это простая таблица ключ-значение. Давайте пройдемся по ней пошагово:

* Каждый тип оператора SQL вроде `CREATE TABLE` имеет подробное описание в [справке по YQL](../yql/reference/index.md).
* `example` - это идентификатор имени таблицы, а `key` и `value` - идентификаторы имен столбцов. Рекомендуется использовать простые имена для таких идентификаторов, но если вам нужно использовать имя с необычными символами, оберните его в обратные кавычки.
* `UInt64` и `String` - это названия типов данных. `String` представляет собой двоичную строку, а `UInt64` - 64-разрядное беззнаковое целое число. Таким образом, наша таблица-пример хранит строковые значения, идентифицируемые беззнаковыми целочисленными ключами. Подробнее [о типах данных](../yql/reference/types/index.md).
* `PRIMARY KEY` - одно из основных понятий SQL, которое оказывает огромное влияние на логику приложения и производительность. В соответствии со стандартом SQL, первичный ключ также подразумевает ограничение уникальности, поэтому таблица не может иметь несколько строк с одинаковыми первичными ключами. В этой примерной таблице довольно просто определить, какой столбец пользователь должен выбрать в качестве первичного ключа, и мы указываем его как `(key)` в круглых скобках после соответствующего ключевого слова. В реальных сценариях таблицы часто содержат десятки столбцов, и первичные ключи могут быть составными (состоять из нескольких столбцов в указанном порядке), поэтому выбор правильного первичного ключа становится больше похожим на искусство. Если вас интересует эта тема, есть [руководство по выбору первичного ключа для максимизации производительности](../best_practices/pk_scalability.md). YDB таблицы обязаны иметь первичный ключ.


## Добавление тестовых данных

Теперь давайте заполним нашу таблицу первыми данными. Самый простой способ - использовать литералы:

```sql
INSERT INTO example (key, value)
VALUES (123, "hello"),
       (321, "world");
```

Пошаговое описание:

* `INSERT INTO` - это классический оператор SQL для добавления новых строк в таблицу. Однако он не является наиболее производительным, так как согласно стандарту SQL он должен проверить, существуют ли в таблице строки с заданными значениями первичного ключа, и выдать ошибку, если они уже есть. Таким образом, если вы запустите этот запрос несколько раз, все попытки, кроме первой, вернут ошибку. Если логика вашего приложения не требует такого поведения, лучше использовать `UPSERT INTO` вместо `INSERT INTO`. Upsert (от "update or insert") будет безусловно записывать предоставленные значения, перезаписывая существующие строки, если они есть. Остальной синтаксис будет таким же.
* `(key, value)` указывает имена столбцов, которые мы вставляем, и их порядок. Предоставленные значения должны соответствовать этому описанию как по количеству столбцов, так и по их типам данных.
* После ключевого слова `VALUES` следует список кортежей, каждый из которых представляет собой строку таблицы. В этом примере у нас есть две строки, идентифицируемые числами 123 и 321 в столбце `key`, и значениями "hello" и "world" в столбце `value` соответственно.

Чтобы убедиться, что строки были действительно добавлены в таблицу, существует распространённый запрос, который в данном случае должен вернуть `2`:

```sql
SELECT COUNT(*) FROM example;
```

Несколько деталей в этом запросе:

* Во `FROM` указывают таблицу, из которой извлекаются данные.
* `COUNT` - это агрегатная функция, подсчитывающая количество значений. По умолчанию, когда нет других специальных выражений вокруг, наличие любой агрегатной функции приводит к сворачиванию результата в одну строку, содержащую агрегаты по всему входным данным (таблице `example` в данном случае).
* Астериск `*` является спецсимволом, который обычно означает "все столбцы"; таким образом, `COUNT` вернет общее количество число строк.

Еще один распространённый способ заполнить таблицу данными - это объединить операции `INSERT INTO` (или `UPSERT INTO`) и `SELECT`. В этом случае значения для сохранения вычисляются внутри базы данных, а не предоставляются клиентом в виде литералов. Для демонстрации этого подхода мы используем немного более реалистичный запрос:

```sql
$subquery = SELECT ListFromRange(1000, 10000) AS keys;

UPSERT INTO example
SELECT
    key,
    CAST(RandomUuid(key) AS String) AS value
FROM $subquery
FLATTEN LIST BY keys AS key
```

В этом запросе происходит немало интересного, давайте рассмотрим его подробнее:

* `$subquery` - это именованное выражение. Этот синтаксис является расширением YQL по сравнению со стандартным SQL и позволяет делать более читаемые сложные запросы. Он работает так же, как если бы вы написали первый `SELECT` по месту, где `$subquery` используется в последней строке. Однако, использование именованных выражений позволяет легче понять, что происходит шаг за шагом, подобно переменным в обычных языках программирования.
* `ListFromRange` - это функция, которая создает список последовательных целых чисел, начиная с значения, указанного в первом аргументе, и заканчивая значением, указанным во втором аргументе. Также есть третий необязательный аргумент, который позволяет пропускать числа с указанным шагом, но мы не используем его в нашем примере — по умолчанию возвращаются все целые числа в указанном диапазоне. `List` является одним из наиболее распространенных [контейнерных типов данных](../yql/reference/types/containers.md).
* `AS` - это ключевое слово, используемое для задания имени значения, которое мы возвращаем из `SELECT`; в данном примере - `keys`.
* В части `FROM ... FLATTEN LIST BY ... AS ...` есть несколько значимых моментов:
  * Другой `SELECT`, использованный в выражении `FROM`, называется подзапросом. Поэтому мы выбрали это имя для нашего именованного выражения `$subquery`, но мы могли бы выбрать что-то более значимое, чтобы объяснить, что это такое. Подзапросы обычно не материализуются; они просто передают вывод одного `SELECT` вводу другого на лету. Они могут использоваться в качестве средства для создания произвольно сложных графов выполнения, особенно в сочетании с другими возможностями YQL.
  * Ключевая фраза `FLATTEN LIST BY` изменяет входные данные, передаваемые через `FROM`, следующим образом: для каждой строки во входных данных она берет столбец типа данных `List` и создает несколько строк в соответствии с количеством элементов в этом списке. Обычно этот столбец списка заменяется столбцом с текущим одиночным элементом, но ключевое слово `AS` в данном контексте позволяет получить доступ и ко всему списку (по исходному имени) и текущему элементу (по имени после `AS`).
* `RandomUuid` - это функция, которая возвращает псевдослучайный [UUID версии 4](https://datatracker.ietf.org/doc/html/rfc4122#section-4.4). В отличие от большинства других функций, она не использует значение своего аргумента (столбец `key`); вместо этого этот аргумент указывает на то, что нам нужно вызывать функцию для каждой строки. Смотрите [ссылку](../yql/reference/builtins/basic.md#random) с дополнительными примерами работы этой функции.
* `CAST(... AS ...)` - это часто используемая функция для преобразования значений в указанный тип данных. В этом контексте после `AS` ожидается указание типа данных (в данном случае `String`), а не произвольного имени.
* `UPSERT INTO` слепо записывает значения в указанные таблицы, как мы обсуждали ранее. При использовании в сочетании с `SELECT` он не требует явного указания имен столбцов `(key, value)`, так как столбцы теперь могут быть просто сопоставлены по именам, возвращённым из `SELECT`.

{% note info "Короткий вопрос!" %}

Что теперь вернёт запрос `SELECT COUNT(*) FROM example;`?

{% endnote %}

## Остановка кластера {#stop}

Остановите локальный кластер {{ ydb-short-name }} после завершения экспериментов:

{% list tabs %}

- Linux x86_64

   Чтобы остановить локальный кластер, выполните следующую команду:

   ```bash
   ~/ydbd/stop.sh
   ```

- Docker

   Чтобы остановить Docker контейнер с локальным кластером, выполните следующую команду:

   ```bash
   docker kill ydb-local
   ```

{% endlist %}

При желании вы можете удалить вашу рабочую директорию с помощью команды `rm -rf ~/ydbd` для очистки файловой системы. Все данные внутри локального кластера {{ ydb-short-name }} будут потеряны.

## Готово! Что дальше?

После освоения базовых операций из этого руководства, пора переходить перейти к более глубоким темам. Выберите то, что кажется наиболее актуальным в зависимости от вашего сценария использования и роли:

* Пройдите более подробный [курс по YQL](../yql/reference/), который сосредоточен на написании запросов.
* Попробуйте создать свое первое приложение для хранения данных в {{ ydb-short-name }} с использованием [одного из SDK](../reference/ydb-sdk/index.md).
* Узнайте, как настроить [развертывание {{ ydb-short-name }} в готовую к промышленной эксплуатации среду](../cluster/index.md).
* Почитайте об используемых в {{ ydb-short-name }} [концепциях](../concepts/index.md).
