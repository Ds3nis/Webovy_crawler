/**
 * Серійна версія веб-краулера без використання MPI
 */

 #include <string>
 #include <vector>
 #include <iostream>
 #include <fstream>
 #include <unordered_set>
 #include <unordered_map>
 #include <queue>
 #include <regex>
 #include <filesystem>
 #include <chrono>
 #include <sstream>
#include <mpi.h>
 #include <iomanip>

 #include "utils.h"
 #include "server.h"


static const std::string MAP_FILE_NAME = "/map.txt";
static const std::string CONTENT_FILE_NAME = "/content.txt";
static const std::string LOG_FILE_NAME = "/log.txt";
static const bool isParallel = false;

// kolikrat se ma provest experiment (a mereni)
constexpr size_t RunCount = 5;

int g_numWorkersA = 0;
int g_numWorkersB = 0;

// Funkce pro měření výkonu - spustí danou funkci několikrát a měří průměrný čas
void Do_Measure(const std::string& name, void(*fnc)())
{
    std::cout << "Measurement: " << name << std::endl;

    // Nejdříve spustíme "naprázdno" pro inicializaci
    std::cout << "Dry run ...";
    fnc();
    std::cout << " OK" << std::endl;

    unsigned long long tm = 0;

    // Provedeme několik opakování měření
    for (size_t i = 0; i < RunCount; i++)
    {
        std::cout << "Run " << i << " ... ";


        auto st = std::chrono::steady_clock::now();

        fnc();

        auto end = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - st).count();
        std::cout << elapsed << "ms" << std::endl;
        tm += elapsed;

        std::cout << "OK" << std::endl;
    }
    // Vypočítáme průměrný čas
    tm /= RunCount;

    std::cout << "Average time: " << tm << "ms" << std::endl << std::endl;
}

enum MpiTags {
    URL_TASK,
    URL_RESULT,
    CONTENT_RESULT,
    TERMINATE
};

 // Структура для зберігання результатів аналізу сторінки
 struct PageAnalysisResult {
     std::string  url;
     std::vector<std::string> foundUrls;
     int imageCount;
     int linkCount;
     int formCount;
     std::vector<std::pair<int, std::string>> headers; // рівень, текст
 };

 // Функція для безпечного перетворення URL в назву файлу
 std::string urlToSafeFilename(const std::string& url) {
     std::string result = url;

     // Видаляємо протокол
     size_t protocolPos = result.find("://");
     if (protocolPos != std::string::npos) {
         result = result.substr(protocolPos + 3);
     }

     // Заміняємо небезпечні символи на підкреслення
     for (char& c : result) {
         if (c == '/' || c == ':' || c == '.' || c == '?' || c == '&' || c == '=' || c == ' ' || c == '#') {
             c = '_';
         }
     }

     return result;
 }

 // Функція для отримання поточної дати/часу у відформатованому вигляді
 std::string getCurrentDateTime() {
     auto now = std::chrono::system_clock::now();
     auto time = std::chrono::system_clock::to_time_t(now);
     std::stringstream ss;
     ss << std::put_time(std::localtime(&time), "%Y_%m_%d_%H_%M");
     return ss.str();
 }

 // Функція для отримання дати/часу у форматі для логу
 std::string getLogDateTime() {
     auto now = std::chrono::system_clock::now();
     auto time = std::chrono::system_clock::to_time_t(now);
     std::stringstream ss;
     ss << std::put_time(std::localtime(&time), "%Y-%m-%d %H:%M:%S");
     return ss.str();
 }

 // Виокремлення базового URL (для перевірки чи URL відноситься до тієї ж домену і шляху)
 std::string getBaseUrl(const std::string& url) {
     std::regex urlRegex("(https?://[^/]+(?:/[^/]+)?)");
     std::smatch match;
     if (std::regex_search(url, match, urlRegex)) {
         return match[1];
     }
     return url;
 }

 // Нормалізація URL відносно базового URL
 std::string normalizeUrl(const std::string& baseUrl, const std::string& url) {
     if (url.empty()) return "";

     // Якщо це абсолютний URL, повертаємо як є
     if (url.find("http://") == 0 || url.find("https://") == 0) {
         return url;
     }

     std::string result = baseUrl;

     // Видаляємо фрагмент URL
     size_t fragmentPos = url.find('#');
     std::string cleanUrl = (fragmentPos != std::string::npos) ? url.substr(0, fragmentPos) : url;

     // Обробка відносних URL
     if (cleanUrl[0] == '/') {
         // URL починається зі слеша, це відносний шлях від кореня домену
         size_t protocolPos = baseUrl.find("://");
         if (protocolPos != std::string::npos) {
             size_t pathStart = baseUrl.find('/', protocolPos + 3);
             if (pathStart != std::string::npos) {
                 result = baseUrl.substr(0, pathStart);
             }
         }
         result += cleanUrl;
     } else {
         // URL не починається зі слеша, це відносний шлях від поточної сторінки
         if (result.back() != '/') {
             size_t lastSlash = result.find_last_of('/');
             if (lastSlash != std::string::npos) {
                 result = result.substr(0, lastSlash + 1);
             } else {
                 result += '/';
             }
         }
         result += cleanUrl;
     }

     return result;
 }

 // Функція для перевірки чи URL належить до тієї ж домену і шляху
 bool isSameDomain(const std::string& baseUrl, const std::string& url) {
     return url.find(baseUrl) == 0;
 }

int calculateImgHtml(const std::regex& imgRegex, const std::string& html) {
     auto imgBegin = std::sregex_iterator(html.begin(), html.end(), imgRegex);
     auto imgEnd = std::sregex_iterator();
     return std::distance(imgBegin, imgEnd);
 }

int calculateFormHtml(const std::regex& formRegex, const std::string& html) {
     auto formBegin = std::sregex_iterator(html.begin(), html.end(), formRegex);
     auto formEnd = std::sregex_iterator();
     return std::distance(formBegin, formEnd);
 }

std::pair<int, std::vector<std::string>> urlProcessingHtml(const std::regex& linkRegex,const std::string& html, const std::string& baseUrl) {
     auto linkBegin = std::sregex_iterator(html.begin(), html.end(), linkRegex);
     auto linkEnd = std::sregex_iterator();
     int numberOfLinks = std::distance(linkBegin, linkEnd);

     std::cout << "Seznam odkazů nalezených na zadané url adrese" << std::endl;
     std::vector<std::string> links;
     for (std::sregex_iterator i = linkBegin; i != linkEnd; ++i) {
         std::smatch match = *i;
         std::string href = match[1];
         std::string normalizedUrl = normalizeUrl(baseUrl, href);

         std::cout << "Cesta ke zdroji URI " << href << std::endl;
         std::cout << "Úplná adresa nalezené stránky " << normalizedUrl << std::endl;

         if (!normalizedUrl.empty() && isSameDomain(baseUrl, normalizedUrl)) {
             links.push_back(normalizedUrl);
         }
     }

     return std::make_pair(numberOfLinks, links);
 }

 // Функція для аналізу HTML-контенту
 PageAnalysisResult analyzeHtml(const std::string& url, const std::string& html) {
     PageAnalysisResult result;
     result.url = url;
     result.imageCount = 0;
     result.linkCount = 0;
     result.formCount = 0;

     std::string baseUrl = getBaseUrl(url);
     std::cout << "url: " << url << "; baseUrl:" << baseUrl << std::endl;

     // Регулярні вирази для пошуку елементів
     std::regex imgRegex("<img[^>]*>");
     std::regex linkRegex("<a[^>]*href=[\"']([^\"']+)[\"'][^>]*>");
     std::regex formRegex("<form[^>]*>");
     std::regex headerRegex("<h([1-6])[^>]*>(.*?)</h\\1>");

     // Підрахунок зображень
     result.imageCount = calculateImgHtml(imgRegex, html);

     // Підрахунок посилань та збір URL
     std::pair<int, std::vector<std::string>> urlProcessing = urlProcessingHtml(linkRegex, html, baseUrl);
     result.linkCount = urlProcessing.first;
     result.foundUrls = urlProcessing.second;

     // Підрахунок форм
     result.formCount = calculateFormHtml(formRegex, html);

     // Аналіз заголовків
     std::string::const_iterator searchStart(html.cbegin());
     std::smatch headerMatch;
     while (std::regex_search(searchStart, html.cend(), headerMatch, headerRegex)) {
         int level = std::stoi(headerMatch[1]);
         std::string headerText = headerMatch[2];

         // Очищення тексту заголовка від тегів
         std::regex tagRegex("<[^>]*>");
         headerText = std::regex_replace(headerText, tagRegex, "");

         result.headers.push_back({level, headerText});
         searchStart = headerMatch.suffix().first;
     }

     return result;
 }

void printVisitedUrls(std::queue<std::string> visitedUrls) {
     int index = 1;
     while (!visitedUrls.empty()) {
         std::cout << "[" << index++ << "] " << visitedUrls.front() << std::endl;
         visitedUrls.pop();
     }
 }


 // Серійна функція для краулінгу
 void serialCrawl(const std::string& startUrl, std::unordered_map<std::string, PageAnalysisResult>& results) {
     auto overallStart = std::chrono::high_resolution_clock::now();
     std::queue<std::string> urlQueue;
     std::unordered_set<std::string> visitedUrls;
     std::string baseUrl = getBaseUrl(startUrl);

     urlQueue.push(startUrl);
     visitedUrls.insert(startUrl);

     int counter = 0;
     unsigned long long tmDownload = 0;
     unsigned long long tmAnalyze = 0;

     while (!urlQueue.empty()) {
         std::string currentUrl = urlQueue.front();
         urlQueue.pop();
         counter++;
         std::cout << "Zahájení zkoumání stránky (serial) z url " << currentUrl << std::endl;

         // Завантаження HTML
         auto start = std::chrono::high_resolution_clock::now();
         std::string html = utils::downloadHTML(currentUrl);
         auto end = std::chrono::high_resolution_clock::now();
         auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
         tmDownload += elapsed.count();
         if (html.empty()) continue;

         // Аналіз сторінки
         auto start1 = std::chrono::high_resolution_clock::now();
         PageAnalysisResult analysis = analyzeHtml(currentUrl, html);
         auto end1 = std::chrono::high_resolution_clock::now();
         auto elapsed1 = std::chrono::duration_cast<std::chrono::milliseconds>(end1 - start1);
         tmAnalyze += elapsed1.count();

         results[currentUrl] = analysis;

         // Додавання нових URL в чергу
         for (const auto& url : analysis.foundUrls) {
             if (visitedUrls.find(url) == visitedUrls.end() && isSameDomain(baseUrl, url)) {
                 urlQueue.push(url);
                 visitedUrls.insert(url);
             }
         }
         printVisitedUrls(urlQueue);
     }
     auto overallEnd = std::chrono::high_resolution_clock::now();
     auto overallElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(overallEnd - overallStart);
     std::cout << "Celkový čas provedení funkce serialCrawl: " << overallElapsed.count() << " ms" << std::endl;

     // tmDownload /= counter;
     // tmAnalyze /= counter;
     std::cout << "Průměrná doba provedení operace analýzy obsahu stránky: " << tmAnalyze << " ms"<< std::endl;
     std::cout << "Průměrný čas pro provedení operace načtení obsahu stránky: " << tmDownload << " ms" << std::endl;
 }

void createWebGraph(const auto& resultDir, const auto& results) {
     std::ofstream mapFile(resultDir + MAP_FILE_NAME);

     // Запис вузлів графа
     for (const auto& pair : results) {
         mapFile << pair.first << std::endl;
     }

     // Запис ребер графа
     for (const auto& pair : results) {
         const std::string& sourceUrl = pair.first;
         for (const std::string& targetUrl : pair.second.foundUrls) {
             if (results.find(targetUrl) != results.end()) {
                 mapFile << sourceUrl << " " << targetUrl << std::endl;
             }
         }
     }
     mapFile.close();
 }

void createContent(const auto& resultDir, const auto& results) {
     std::ofstream contentFile(resultDir + CONTENT_FILE_NAME);
     for (const auto& pair : results) {
         contentFile << pair.first << std::endl;
         contentFile << "IMAGES " << pair.second.imageCount << std::endl;
         contentFile << "LINKS " << pair.second.linkCount << std::endl;
         contentFile << "FORMS " << pair.second.formCount << std::endl;

         for (const auto& header : pair.second.headers) {
             for (int i = 0; i < header.first; i++) {
                 contentFile << "-";
             }
             contentFile << " " << header.second << std::endl;
         }
         contentFile << std::endl;
     }
     contentFile.close();
 }

void createLog(const auto& resultDir, const auto& results, const auto& startTime) {
     std::string endTime = getLogDateTime();
     std::ofstream logFile(resultDir + LOG_FILE_NAME);
     logFile << startTime << std::endl;
     logFile << endTime << std::endl;
     logFile << "OK" << std::endl;
     logFile.close();
 }


 // Функція для серійної обробки списку URL
 void processSerial(const std::vector<std::string>& URLs, std::string& vystup) {
     static std::string startTime = getLogDateTime();
     auto start = std::chrono::system_clock::now();
     vystup = "<h2>Результати краулінгу (серійна версія)</h2><ul>";
     // Створення каталогу для результатів
     std::filesystem::create_directory("results");

     // Обробка кожного URL
     for (const auto& url : URLs) {
         static std::string curr_url = url;
         static std::unordered_map<std::string, PageAnalysisResult> results;

        //  Виконання краулінгу
        //  Do_Measure("Crawling " + url, []() {
        //     serialCrawl(curr_url, results);
        // });

         serialCrawl(url, results);

         // Створення каталогу для результатів цього URL
         std::string safeUrlName = urlToSafeFilename(url);
         std::string resultDirName = getCurrentDateTime() + "_" + safeUrlName;
         static std::string resultDir = "results/" + resultDirName;
         std::filesystem::create_directory(resultDir);

         // Запис файлів з результатами
         // 1. map.txt - граф сторінок
         // Do_Measure("Zápis grafu stranek do souboru " + MAP_FILE_NAME, []() {
         //     createWebGraph(resultDir, results);
         // });
         createWebGraph(resultDir, results);

         // 2. content.txt - інформація про вміст сторінок
        //  Do_Measure("Zápis obsahu stranek do souboru " + CONTENT_FILE_NAME, []() {
        //      createContent(resultDir, results);
        // });
         createContent(resultDir, results);

         // 3. log.txt - журнал виконання
        //  Do_Measure("Zápis log do souboru " + LOG_FILE_NAME, []() {
        //      createLog(resultDir, results, startTime);
        // });
         createLog(resultDir, results, startTime);

         vystup += "<li>Оброблено URL: " + url + " - результати збережено в " + resultDirName + "</li>";
     }

     vystup += "</ul>";
     auto end = std::chrono::system_clock::now();
     auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();;
     std::cout << "Total time: " << elapsed << " ms" << std::endl;
 }


 // Майстер процес - розподіляє роботу і збирає результати
void masterProcess(const std::vector<std::string>& URLs, int numWorkerA, int numWorkerB, std::string& output) {
    int numUrls = URLs.size();

    // Перевіряємо, чи є що розподіляти
    if (numUrls == 0) {
        output = "<h2>Немає URL для обробки</h2>";

        // Повідомляємо всім воркерам A про завершення
        for (int i = 1; i <= numWorkerA; ++i) {
            int terminate = -1;
            MPI_Send(&terminate, 1, MPI_INT, i, URL_TASK, MPI_COMM_WORLD);
        }
        return;
    }

    std::string startTime = getLogDateTime();
    std::cout << "Master: Starting processing " << numUrls << " URLs" << std::endl;

    // Розподіл URL між воркерами A
    for (int i = 0; i < numUrls; ++i) {
        int workerA = (i % numWorkerA) + 1; // Worker A має ID від 1 до N

        // Відправка URL до воркера A
        std::string url = URLs[i];
        int urlLength = url.length();

        std::cout << "Master: Sending URL to worker A " << workerA << ": " << url << std::endl;

        MPI_Send(&urlLength, 1, MPI_INT, workerA, URL_TASK, MPI_COMM_WORLD);
        MPI_Send(url.c_str(), urlLength, MPI_CHAR, workerA, URL_TASK, MPI_COMM_WORLD);
    }

    // Повідомлення всім воркерам A про завершення розподілу задач
    for (int i = 1; i <= numWorkerA; ++i) {
        int terminate = -1;
        std::cout << "Master: Sending termination signal to worker A " << i << std::endl;
        MPI_Send(&terminate, 1, MPI_INT, i, URL_TASK, MPI_COMM_WORLD);
    }

    // Створення каталогу для результатів
    std::filesystem::create_directory("results");

    // Отримання результатів від воркерів A
    output = "<h2>Результати краулінгу</h2><ul>";

    for (int i = 0; i < numUrls; ++i) {
        // Отримуємо результати від будь-якого воркера A
        MPI_Status status;
        int mapSizeReceived;

        std::cout << "Master: Waiting for results #" << (i+1) << " of " << numUrls << std::endl;

        MPI_Recv(&mapSizeReceived, 1, MPI_INT, MPI_ANY_SOURCE, URL_RESULT, MPI_COMM_WORLD, &status);

        int workerA = status.MPI_SOURCE;
        std::cout << "Master: Received map size from worker A " << workerA << ": " << mapSizeReceived << std::endl;

        char* mapBuffer = new char[mapSizeReceived + 1];
        MPI_Recv(mapBuffer, mapSizeReceived, MPI_CHAR, workerA, URL_RESULT, MPI_COMM_WORLD, &status);
        mapBuffer[mapSizeReceived] = '\0';
        std::string mapData(mapBuffer);
        delete[] mapBuffer;

        int contentSizeReceived;
        MPI_Recv(&contentSizeReceived, 1, MPI_INT, workerA, CONTENT_RESULT, MPI_COMM_WORLD, &status);

        std::cout << "Master: Received content size from worker A " << workerA << ": " << contentSizeReceived << std::endl;

        char* contentBuffer = new char[contentSizeReceived + 1];
        MPI_Recv(contentBuffer, contentSizeReceived, MPI_CHAR, workerA, CONTENT_RESULT, MPI_COMM_WORLD, &status);
        contentBuffer[contentSizeReceived] = '\0';
        std::string contentData(contentBuffer);
        delete[] contentBuffer;

        // Отримуємо URL, для якого воркер надіслав результати
        int urlSizeReceived;
        MPI_Recv(&urlSizeReceived, 1, MPI_INT, workerA, URL_RESULT, MPI_COMM_WORLD, &status);

        char* urlBuffer = new char[urlSizeReceived + 1];
        MPI_Recv(urlBuffer, urlSizeReceived, MPI_CHAR, workerA, URL_RESULT, MPI_COMM_WORLD, &status);
        urlBuffer[urlSizeReceived] = '\0';
        std::string crawledUrl(urlBuffer);
        delete[] urlBuffer;

        std::cout << "Master: Processed URL: " << crawledUrl << std::endl;

        // Створення каталогу для результатів цього URL
        std::string safeUrlName = urlToSafeFilename(crawledUrl);
        std::string resultDirName = getCurrentDateTime() + "_" + safeUrlName;
        std::string resultDir = "results/" + resultDirName;
        std::filesystem::create_directory(resultDir);

        // Запис файлів з результатами
        std::ofstream mapFile(resultDir + "/map.txt");
        mapFile << mapData;
        mapFile.close();

        std::ofstream contentFile(resultDir + "/content.txt");
        contentFile << contentData;
        contentFile.close();

        std::string endTime = getLogDateTime();

        std::ofstream logFile(resultDir + "/log.txt");
        logFile << startTime << std::endl;
        logFile << endTime << std::endl;
        logFile << "OK" << std::endl;
        logFile.close();

        output += "<li>Оброблено URL: " + crawledUrl + " - результати збережено в " + resultDirName + "</li>";
    }

    output += "</ul>";
    std::cout << "Master: All URLs processed successfully" << std::endl;
}


// Worker A - керує групою Worker B і відповідає за одну домену
void workerA(int myRank, int numWorkerB, int numWorkerA) {
    int firstWorkerB = (numWorkerB * myRank) - numWorkerB + numWorkerA + 1;
    std::vector<int> availableWorkersB;
    int busyWorkersB = 0; // Лічильник занятих Worker B

    std::cout << "Worker A " << myRank << ": Starting with first Worker B = " << firstWorkerB << std::endl;

    // Ініціалізація списку доступних Worker B
    for (int i = 0; i < numWorkerB; ++i) {
        availableWorkersB.push_back(firstWorkerB + i);
    }

    while (true) {
        MPI_Status status;
        int urlLength;

        std::cout << "Worker A " << myRank << ": Waiting for URL task" << std::endl;
        MPI_Recv(&urlLength, 1, MPI_INT, 0, URL_TASK, MPI_COMM_WORLD, &status);

        // Перевірка на сигнал завершення
        if (urlLength == -1) {
            std::cout << "Worker A " << myRank << ": Received termination signal" << std::endl;
            break;
        }

        // Отримання URL
        char* urlBuffer = new char[urlLength + 1];
        MPI_Recv(urlBuffer, urlLength, MPI_CHAR, 0, URL_TASK, MPI_COMM_WORLD, &status);
        urlBuffer[urlLength] = '\0';
        std::string startUrl(urlBuffer);
        delete[] urlBuffer;

        std::cout << "Worker A " << myRank << ": Processing URL: " << startUrl << std::endl;

        // Структури даних для відстеження обходу
        std::queue<std::string> urlQueue;
        std::unordered_set<std::string> visitedUrls;
        std::unordered_map<std::string, PageAnalysisResult> results;
        std::string baseUrl = getBaseUrl(startUrl);

        urlQueue.push(startUrl);
        visitedUrls.insert(startUrl);

        int processedUrls = 0;
        int maxUrlsToProcess = 100; // Обмеження для уникнення нескінченного обходу

        // Обробка всіх URL для цієї домени
        while ((!urlQueue.empty() || busyWorkersB > 0) && processedUrls < maxUrlsToProcess) {
            // Призначаємо роботу доступним Worker B, якщо є URL в черзі
            while (!urlQueue.empty() && !availableWorkersB.empty() && processedUrls < maxUrlsToProcess) {
                std::string currentUrl = urlQueue.front();
                urlQueue.pop();

                // Отримання доступного Worker B
                int workerB = availableWorkersB.back();
                availableWorkersB.pop_back();
                busyWorkersB++; // Збільшуємо лічильник занятих Worker B

                std::cout << "Worker A " << myRank << ": Assigning URL to Worker B " << workerB
                          << ": " << currentUrl << " (busy workers: " << busyWorkersB << ")" << std::endl;

                // Відправка URL до Worker B
                int urlLength = currentUrl.length();
                MPI_Send(&urlLength, 1, MPI_INT, workerB, URL_TASK, MPI_COMM_WORLD);
                MPI_Send(currentUrl.c_str(), urlLength, MPI_CHAR, workerB, URL_TASK, MPI_COMM_WORLD);
            }

            // Очікуємо результат від Worker B, якщо є зайняті воркери
            if (busyWorkersB > 0) {
                // Отримання результату від Worker B і додавання його назад у список доступних
                int workerB;

                std::cout << "Worker A " << myRank << ": Waiting for any Worker B to finish (busy: "
                          << busyWorkersB << ")" << std::endl;
                MPI_Status b_status;
                MPI_Recv(&workerB, 1, MPI_INT, MPI_ANY_SOURCE, TERMINATE, MPI_COMM_WORLD, &b_status);

                workerB = b_status.MPI_SOURCE; // використовуємо реальне джерело повідомлення
                availableWorkersB.push_back(workerB);
                busyWorkersB--; // Зменшуємо лічильник занятих Worker B

                std::cout << "Worker A " << myRank << ": Worker B " << workerB
                          << " finished (remaining busy: " << busyWorkersB << ")" << std::endl;

                // Отримання результатів аналізу
                int urlLength;
                MPI_Recv(&urlLength, 1, MPI_INT, workerB, URL_RESULT, MPI_COMM_WORLD, &b_status);

                char* urlBuffer = new char[urlLength + 1];
                MPI_Recv(urlBuffer, urlLength, MPI_CHAR, workerB, URL_RESULT, MPI_COMM_WORLD, &b_status);
                urlBuffer[urlLength] = '\0';
                std::string analyzedUrl(urlBuffer);
                delete[] urlBuffer;

                std::cout << "Worker A " << myRank << ": Received analysis for URL: " << analyzedUrl << std::endl;

                // Отримання кількості зображень, посилань, форм
                int imageCount, linkCount, formCount, headersCount;
                MPI_Recv(&imageCount, 1, MPI_INT, workerB, CONTENT_RESULT, MPI_COMM_WORLD, &b_status);
                MPI_Recv(&linkCount, 1, MPI_INT, workerB, CONTENT_RESULT, MPI_COMM_WORLD, &b_status);
                MPI_Recv(&formCount, 1, MPI_INT, workerB, CONTENT_RESULT, MPI_COMM_WORLD, &b_status);
                MPI_Recv(&headersCount, 1, MPI_INT, workerB, CONTENT_RESULT, MPI_COMM_WORLD, &b_status);

                // Отримання заголовків
                PageAnalysisResult result;
                result.url = analyzedUrl;
                result.imageCount = imageCount;
                result.linkCount = linkCount;
                result.formCount = formCount;

                for (int i = 0; i < headersCount; i++) {
                    int level;
                    MPI_Recv(&level, 1, MPI_INT, workerB, CONTENT_RESULT, MPI_COMM_WORLD, &b_status);

                    int textLength;
                    MPI_Recv(&textLength, 1, MPI_INT, workerB, CONTENT_RESULT, MPI_COMM_WORLD, &b_status);

                    char* textBuffer = new char[textLength + 1];
                    MPI_Recv(textBuffer, textLength, MPI_CHAR, workerB, CONTENT_RESULT, MPI_COMM_WORLD, &b_status);
                    textBuffer[textLength] = '\0';

                    result.headers.push_back({level, std::string(textBuffer)});
                    delete[] textBuffer;
                }

                // Отримання знайдених URL
                int urlsCount;
                MPI_Recv(&urlsCount, 1, MPI_INT, workerB, URL_RESULT, MPI_COMM_WORLD, &b_status);

                std::cout << "Worker A " << myRank << ": URL " << analyzedUrl << " has " << urlsCount << " links" << std::endl;

                for (int i = 0; i < urlsCount; i++) {
                    int foundUrlLength;
                    MPI_Recv(&foundUrlLength, 1, MPI_INT, workerB, URL_RESULT, MPI_COMM_WORLD, &b_status);

                    char* foundUrlBuffer = new char[foundUrlLength + 1];
                    MPI_Recv(foundUrlBuffer, foundUrlLength, MPI_CHAR, workerB, URL_RESULT, MPI_COMM_WORLD, &b_status);
                    foundUrlBuffer[foundUrlLength] = '\0';
                    std::string foundUrl(foundUrlBuffer);
                    delete[] foundUrlBuffer;

                    result.foundUrls.push_back(foundUrl);

                    // Додавання нових URL в чергу
                    if (visitedUrls.find(foundUrl) == visitedUrls.end() && isSameDomain(baseUrl, foundUrl)) {
                        urlQueue.push(foundUrl);
                        visitedUrls.insert(foundUrl);
                    }
                }

                results[analyzedUrl] = result;
                processedUrls++;
            } else if (urlQueue.empty()) {
                // Якщо немає більше URL в черзі і немає занятих Worker B, виходимо з циклу
                break;
            }
        }

        // Повідомлення про завершення для всіх воркерів B
        std::cout << "Worker A " << myRank << ": Sending termination to all Worker B processes" << std::endl;
        for (int i = 0; i < numWorkerB; i++) {
            int currentWorkerB = firstWorkerB + i;
            int terminate = -1;
            MPI_Send(&terminate, 1, MPI_INT, currentWorkerB, URL_TASK, MPI_COMM_WORLD);
        }

        // Очікування завершення всіх Worker B
        while (availableWorkersB.size() < numWorkerB) {
            int workerB;
            MPI_Status b_status;
            MPI_Recv(&workerB, 1, MPI_INT, MPI_ANY_SOURCE, TERMINATE, MPI_COMM_WORLD, &b_status);

            workerB = b_status.MPI_SOURCE;
            bool alreadyAvailable = false;
            for (int wb : availableWorkersB) {
                if (wb == workerB) {
                    alreadyAvailable = true;
                    break;
                }
            }

            if (!alreadyAvailable) {
                availableWorkersB.push_back(workerB);
                std::cout << "Worker A " << myRank << ": Worker B " << workerB << " finished (final)" << std::endl;
            }
        }

        // Підготовка результатів для відправки назад майстру
        std::stringstream mapSs, contentSs;

        // Обмеження кількості URL для майстра (якщо їх забагато)
        int maxUrlsToReport = 1000;
        std::vector<std::string> urlsToReport;

        for (const auto& pair : results) {
            if (urlsToReport.size() < maxUrlsToReport) {
                urlsToReport.push_back(pair.first);
            }
        }

        // Запис вузлів графа
        for (const auto& url : urlsToReport) {
            mapSs << url << std::endl;
        }

        // Запис ребер графа
        for (const auto& url : urlsToReport) {
            const auto& result = results[url];
            for (const std::string& targetUrl : result.foundUrls) {
                if (results.find(targetUrl) != results.end()) {
                    mapSs << url << " " << targetUrl << std::endl;
                }
            }
        }

        // Запис даних про контент
        for (const auto& url : urlsToReport) {
            const auto& result = results[url];
            contentSs << url << std::endl;
            contentSs << "IMAGES " << result.imageCount << std::endl;
            contentSs << "LINKS " << result.linkCount << std::endl;
            contentSs << "FORMS " << result.formCount << std::endl;

            for (const auto& header : result.headers) {
                for (int i = 0; i < header.first; i++) {
                    contentSs << "-";
                }
                contentSs << " " << header.second << std::endl;
            }
            contentSs << std::endl;
        }

        // Відправка результатів до майстра
        std::string mapData = mapSs.str();
        int mapSize = mapData.length();

        std::cout << "Worker A " << myRank << ": Sending map data to master (" << mapSize << " bytes)" << std::endl;

        MPI_Send(&mapSize, 1, MPI_INT, 0, URL_RESULT, MPI_COMM_WORLD);
        MPI_Send(mapData.c_str(), mapSize, MPI_CHAR, 0, URL_RESULT, MPI_COMM_WORLD);

        std::string contentData = contentSs.str();
        int contentSize = contentData.length();

        std::cout << "Worker A " << myRank << ": Sending content data to master (" << contentSize << " bytes)" << std::endl;

        MPI_Send(&contentSize, 1, MPI_INT, 0, CONTENT_RESULT, MPI_COMM_WORLD);
        MPI_Send(contentData.c_str(), contentSize, MPI_CHAR, 0, CONTENT_RESULT, MPI_COMM_WORLD);

        // Відправка оригінального URL
        int urlSize = startUrl.length();

        std::cout << "Worker A " << myRank << ": Sending original URL to master: " << startUrl << std::endl;

        MPI_Send(&urlSize, 1, MPI_INT, 0, URL_RESULT, MPI_COMM_WORLD);
        MPI_Send(startUrl.c_str(), urlSize, MPI_CHAR, 0, URL_RESULT, MPI_COMM_WORLD);
    }

    std::cout << "Worker A " << myRank << ": Exiting" << std::endl;
}

void workerB(int myRank, int masterA) {
    std::cout << "Worker B " << myRank << ": Starting with master A = " << masterA << std::endl;

    while (true) {
        MPI_Status status;
        int urlLength;

        std::cout << "Worker B " << myRank << ": Waiting for URL task" << std::endl;
        MPI_Recv(&urlLength, 1, MPI_INT, masterA, URL_TASK, MPI_COMM_WORLD, &status);

        // Перевірка на сигнал завершення
        if (urlLength == -1) {
            std::cout << "Worker B " << myRank << ": Received termination signal" << std::endl;
            break;
        }

        // Отримання URL
        char* urlBuffer = new char[urlLength + 1];
        MPI_Recv(urlBuffer, urlLength, MPI_CHAR, masterA, URL_TASK, MPI_COMM_WORLD, &status);
        urlBuffer[urlLength] = '\0';
        std::string url(urlBuffer);
        delete[] urlBuffer;

        std::cout << "Worker B " << myRank << ": Processing URL: " << url << std::endl;

        // Завантаження і аналіз HTML
        std::string html;
        html = utils::downloadHTML(url);
        std::cout << "Worker B " << myRank << ": Downloaded HTML of size: " << html.length() << std::endl;

        PageAnalysisResult result;
        result = analyzeHtml(url, html);
        std::cout << "Worker B " << myRank << ": Analyzed HTML, found " << result.foundUrls.size() << " URLs" << std::endl;
        // Захист від завеликих даних
        const int MAX_URLS = 100;
        if (result.foundUrls.size() > MAX_URLS) {
            std::cout << "Worker B " << myRank << ": Limiting found URLs from " << result.foundUrls.size() << " to " << MAX_URLS << std::endl;
            result.foundUrls.resize(MAX_URLS);
        }

        // Відправка результатів назад до Worker A
        std::cout << "Worker B " << myRank << ": Sending results back to Worker A " << masterA << std::endl;
        MPI_Send(&myRank, 1, MPI_INT, masterA, TERMINATE, MPI_COMM_WORLD);

        // Відправка URL
        urlLength = result.url.length();
        MPI_Send(&urlLength, 1, MPI_INT, masterA, URL_RESULT, MPI_COMM_WORLD);
        MPI_Send(result.url.c_str(), urlLength, MPI_CHAR, masterA, URL_RESULT, MPI_COMM_WORLD);

        // Відправка інформації про контент
        MPI_Send(&result.imageCount, 1, MPI_INT, masterA, CONTENT_RESULT, MPI_COMM_WORLD);
        MPI_Send(&result.linkCount, 1, MPI_INT, masterA, CONTENT_RESULT, MPI_COMM_WORLD);
        MPI_Send(&result.formCount, 1, MPI_INT, masterA, CONTENT_RESULT, MPI_COMM_WORLD);

        // Відправка заголовків
        int headersCount = result.headers.size();
        MPI_Send(&headersCount, 1, MPI_INT, masterA, CONTENT_RESULT, MPI_COMM_WORLD);

        for (const auto& header : result.headers) {
            MPI_Send(&header.first, 1, MPI_INT, masterA, CONTENT_RESULT, MPI_COMM_WORLD);

            int textLength = header.second.length();
            MPI_Send(&textLength, 1, MPI_INT, masterA, CONTENT_RESULT, MPI_COMM_WORLD);
            MPI_Send(header.second.c_str(), textLength, MPI_CHAR, masterA, CONTENT_RESULT, MPI_COMM_WORLD);
        }

        // Відправка знайдених URL
        int urlsCount = result.foundUrls.size();
        MPI_Send(&urlsCount, 1, MPI_INT, masterA, URL_RESULT, MPI_COMM_WORLD);

        for (const std::string& foundUrl : result.foundUrls) {
            int foundUrlLength = foundUrl.length();
            MPI_Send(&foundUrlLength, 1, MPI_INT, masterA, URL_RESULT, MPI_COMM_WORLD);
            MPI_Send(foundUrl.c_str(), foundUrlLength, MPI_CHAR, masterA, URL_RESULT, MPI_COMM_WORLD);
        }
    }

    // Повідомляємо Worker A, що ми завершили роботу
    std::cout << "Worker B " << myRank << ": Sending final termination to Worker A " << masterA << std::endl;
    MPI_Send(&myRank, 1, MPI_INT, masterA, TERMINATE, MPI_COMM_WORLD);
    std::cout << "Worker B " << myRank << ": Exiting" << std::endl;
}

void processParallel(const std::vector<std::string>& URLs, std::string& vystup) {
     int rank, size;
     MPI_Comm_rank(MPI_COMM_WORLD, &rank);
     MPI_Comm_size(MPI_COMM_WORLD, &size);

     // Визначення кількості воркерів A і B з розміру MPI_COMM_WORLD
     // Припускаємо, що параметри -n і -m вже оброблені в main() і кількість процесів правильна

     // Парсинг аргументів командного рядка для отримання N і M
     int numWorkerA = 0;
     int numWorkerB = 0;

     // Майстер повинен знати ці значення, тому вони передаються через MPI_Bcast
     if (rank == 0) {
         // Беремо значення з глобальних змінних, які встановлюються в main()
         extern int g_numWorkersA;
         extern int g_numWorkersB;
         numWorkerA = g_numWorkersA;
         numWorkerB = g_numWorkersB;

         std::cout << "Master: Using " << numWorkerA << " Workers A and " << numWorkerB << " Workers B per Worker A" << std::endl;
     }

     // Поширюємо значення N і M всім процесам
     MPI_Bcast(&numWorkerA, 1, MPI_INT, 0, MPI_COMM_WORLD);
     MPI_Bcast(&numWorkerB, 1, MPI_INT, 0, MPI_COMM_WORLD);

     // Розподіл ролей між процесами
     if (rank == 0) {
         // Майстер процес
         masterProcess(URLs, numWorkerA, numWorkerB, vystup);
     } else if (rank <= numWorkerA) {
         // Worker A процес
         workerA(rank, numWorkerB, numWorkerA);
         vystup = ""; // Worker процеси не повертають HTML
     } else {
         // Worker B процес
         int workerAId = ((rank - numWorkerA - 1) / numWorkerB) + 1;
         workerB(rank, workerAId);
         vystup = ""; // Worker процеси не повертають HTML
     }
 }

int main(int argc, char** argv) {

	// inicializace serveru
	CServer svr;
     if (isParallel) {
         if (argc != 5) {
             std::cerr << "Error: Invalid number of arguments!" << std::endl;
             return EXIT_FAILURE;
         }

         MPI_Init(&argc, &argv);

         int world_size = 10;
         int rank = 0;
         MPI_Comm_size(MPI_COMM_WORLD, &world_size);
         MPI_Comm_rank(MPI_COMM_WORLD, &rank);

         char processor_name[MPI_MAX_PROCESSOR_NAME];
         int name_len;
         MPI_Get_processor_name(processor_name, &name_len);

         std::cout << "Привіт від процесу " << rank << " з " << world_size
                   << " на вузлі " << processor_name << std::endl;

         g_numWorkersA = 0;
         g_numWorkersB = 0;


         int result = EXIT_FAILURE;
         if (rank == 0) {
             for (int i = 1; i < argc; i += 2) {
                 std::string arg = argv[i];
                 if (arg == "-n" && i + 1 < argc) {
                     g_numWorkersA = std::atoi(argv[i + 1]);
                 } else if (arg == "-m" && i + 1 < argc) {
                     g_numWorkersB = std::atoi(argv[i + 1]);
                 } else {
                     std::cerr << "Error: Unknown argument " << arg << std::endl;
                     std::cerr << "Usage: " << argv[0] << " -n <num_workers_A> -m <num_workers_B>" << std::endl;
                     MPI_Finalize();
                     return EXIT_FAILURE;
                 }
             }

             int expectedProcesses = 1 + g_numWorkersA + (g_numWorkersA * g_numWorkersB);
             if (world_size != expectedProcesses) {
                 if (rank == 0) {
                     std::cerr << "Error: Incorrect number of MPI processes. Expected " << expectedProcesses
                               << " but got " << world_size << std::endl;
                     std::cerr << "For -n " << g_numWorkersA << " -m " << g_numWorkersB
                               << " you need: 1 master + " << g_numWorkersA << " workers A + "
                               << g_numWorkersA * g_numWorkersB << " workers B = " << expectedProcesses << " processes." << std::endl;
                 }
                 MPI_Finalize();
                 return EXIT_FAILURE;
             }


             if (!svr.Init("../data", "0.0.0.0", 8001)) {
                 std::cerr << "Nelze inicializovat server!" << std::endl;
                 return EXIT_FAILURE;
             }

             // registrace callbacku pro zpracovani odeslanych URL
             svr.RegisterFormCallback(processParallel);

             result = svr.Run() ? EXIT_SUCCESS : EXIT_FAILURE;
         }else {
             std::string dummy;
             processParallel(std::vector<std::string>(), dummy);
             result = EXIT_SUCCESS;
         }

         MPI_Finalize();
         return result;

     }else {
         if (!svr.Init("../data", "localhost", 8001)) {
             std::cerr << "Nelze inicializovat server!" << std::endl;
             return EXIT_FAILURE;
         }

         // registrace callbacku pro zpracovani odeslanych URL
         svr.RegisterFormCallback(processSerial);

         // spusteni serveru
         return svr.Run() ? EXIT_SUCCESS : EXIT_FAILURE;
     }
}
