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
static const bool isParallel = true;

// kolikrat se ma provest experiment (a mereni)
constexpr size_t RunCount = 5;

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

void processParallel(const std::vector<std::string>& URLs, std::string& vystup) {

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

         int numWorkersA = 0;
         int numWorkersB = 0;


         int result = EXIT_FAILURE;
         if (rank == 0) {
             for (int i = 1; i < argc; i += 2) {
                 std::string arg = argv[i];
                 if (arg == "-n" && i + 1 < argc) {
                     numWorkersA = std::atoi(argv[i + 1]);
                 } else if (arg == "-m" && i + 1 < argc) {
                     numWorkersB = std::atoi(argv[i + 1]);
                 } else {
                     std::cerr << "Error: Unknown argument " << arg << std::endl;
                     std::cerr << "Usage: " << argv[0] << " -n <num_workers_A> -m <num_workers_B>" << std::endl;
                     MPI_Finalize();
                     return EXIT_FAILURE;
                 }
             }

             int expectedProcesses = 1 + numWorkersA + (numWorkersA * numWorkersB);
             if (world_size != expectedProcesses) {
                 if (rank == 0) {
                     std::cerr << "Error: Incorrect number of MPI processes. Expected " << expectedProcesses
                               << " but got " << world_size << std::endl;
                     std::cerr << "For -n " << numWorkersA << " -m " << numWorkersB
                               << " you need: 1 master + " << numWorkersA << " workers A + "
                               << numWorkersA * numWorkersB << " workers B = " << expectedProcesses << " processes." << std::endl;
                 }
                 MPI_Finalize();
                 return EXIT_FAILURE;
             }

             std::cout << numWorkersA << " " << numWorkersB << std::endl;

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
