#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "CX_Aureus.h"
#include "Aureus.h"
#include "AureusVideo.h"
#include "AureusHeads.h"
#include "AureusGallery.h"
#include "AureusImage.h"
#include "cxutils.h"
#include <atomic>
#include <filesystem>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>
#include <fstream>
#include <thread>
#include <mutex>
#include <cmath>
#include <functional>
#include <cxxopts.hpp>
#include <csignal>
#include <cstdlib>
#include "npy.hpp"

CX_AUREUS p_aureus;
std::mutex matchingMutex;

// Atomic variable threads check to see if they need to exit
std::aromic<bool> exitThreads(false);

void freeAureus() {
    char msg[1024];
    if (!CX_FreeAureus(p_aureus, msg)) {
        printf("Failed to free Aureus:\n%s\n", msg);
    } else {
        printf("[INFO] Successfully freed Aureus!\n");
    }
}

// Function to be executed on SIGINT (Ctrl+C) or SIGTERM
void signalHandler(int signal) {
  exitThreads.store(true);    //If a signal is received, tell all running threads to exit

  switch (signal) {
    case SIGTERM:
      std::cout << "\n[INFO] SIGTERM received. Freeing Aureus." << std::endl;
      break;
    case SIGINT:
      std::cout << "\n[INFO] SIGINT received. Freeing Aureus." << std::endl;
      break;
  }

  // We wait to call freeAureus() until all the threads have joined in main()

  // freeAureus();
  // std::cout << "[INFO] Exiting the program." << std::endl;
  // exit(EXIT_SUCCESS); // Terminate the program
}


// Function to handle thread-specific signals (SIGSEGV and SIGABRT)
void threadSignalHandler(int signal) {
    quitThreads.store(true);
    std::cout << "\nReceived signal: ";
    switch (signal) {
        case SIGSEGV:
            std::cout << "SIGSEGV" << std::endl;
            freeAureus();
            exit(EXIT_FAILURE);
            break;
        case SIGABRT:
            std::cout << "SIGABRT" << std::endl;
            freeAureus();
            exit(EXIT_FAILURE);
            break; 
    }
}

void Aureus_Init_CallBack(cx_real percent, const char* info, void* p_object){
  printf("%f%% %s\n", percent, info);
}

void FRupdateCallback(const char* msg, cx_int total, cx_int current, void* p_object){
  if (total > 0) printf("Updating FR %s %d/%d   \r", msg, current, total);
}

std::vector<std::string> splitString(const std::string &input, char delimiter) {
  std::vector<std::string> parts;
  parts.reserve(3);
  size_t start = 0, end;

  while ((end = input.find(delimiter, start)) != std::string::npos) {
    parts.push_back(input.substr(start, end - start));
    start = end + 1; // Move start to the position after the delimiter
  }

  // Add the last part of the string
  parts.push_back(input.substr(start));

  return parts;
}

std::vector<std::string> generateCombinations(const std::vector<std::string> &probePaths) {
  std::vector<std::string> combinations;
  long n = probePaths.size();
  combinations.reserve((n * (n-1)) / 2);
  for (int i = 0; i < n; ++i) {
    for (int j = i + 1; j < n; ++j) {
      combinations.push_back(probePaths[i] + "," + probePaths[j]);
    }
  }
  return combinations;
}

std::string getStemFromFilePath(const std::string path) {
  std::filesystem::path filePath(path);
  std::string fileNameWithoutExtension = filePath.stem().string();
  return fileNameWithoutExtension;
}

std::vector<std::string> generateCombinations(const std::vector<std::string>& probePaths, const std::vector<std::string>& galleryPaths) {
  std::vector<std::string> combinations;
  combinations.reserve(probePaths.size() * galleryPaths.size());
  for (int i = 0; i < probePaths.size(); ++i) {
    std::string probeImage = getStemFromFilePath(probePaths[i]);
    for (int j = 0; j < galleryPaths.size(); ++j) {
      std::string galleryImage = getStemFromFilePath(galleryPaths[j]);

      // This portion makes sure that the same instance of a subject is removed.
      // For example, if the gallery has a "ABC_123" and the probes have "ABC_123_cloaked"
      // then it will not be considered
      if (probeImage.find(galleryImage) == std::string::npos){ 
        combinations.push_back(probePaths[i] + "," + galleryPaths[j]);
      }
    }
  }
  return combinations;
}

std::vector<std::string> getFilesWithExtensions(const std::string& directoryPath, const std::vector<std::string>& extensions) {
  std::vector<std::string> matchingFiles;

  try {
    for (const auto& entry : std::filesystem::recursive_directory_iterator(directoryPath)) {
      if (std::filesystem::is_regular_file(entry)){
        std::string entryExtension = entry.path().extension();
        for (const std::string& targetExtension : extensions){
          if (entryExtension == targetExtension){
            matchingFiles.push_back(entry.path().string());
            break;
          }
        }
      } 
    }
  } catch (const std::exception& e) {
      std::cerr << "Error: " << e.what() << std::endl;
  }

  return matchingFiles;
}

std::unordered_map<std::string, cx_byte*> readTemplates(std::vector<std::string> paths, int templateSize){
  std::unordered_map<std::string, cx_byte*> data;
  data.reserve(paths.size());
  for (const std::string& path : paths){
    std::ifstream file(path.c_str(), std::ios::in | std::ios::binary);
    if (file.is_open()){
      cx_byte* pTemplate = new cx_byte[templateSize];
      file.read(reinterpret_cast<char*>(pTemplate), templateSize);
      file.close();
      data[getStemFromFilePath(path)] = pTemplate;
    }
  }
  return data;
}

void processCombination(
  int threadNumber,
  std::vector<std::string>& combinations,
  std::unordered_map<std::string, cx_byte*>& probeData,
  std::unordered_map<std::string, std::unordered_map<std::string, float>>& simMat,
  CX_AUREUS* p_aureus,
  CX_DetectionParams* fdp,
  int templateSize,
  char* msg,
  int modulus,
  unsigned long start,
  unsigned long end,
  std::unordered_map<std::string, cx_byte*>* galleryData = nullptr) {
  
  //Setup the SIGSEGV and SIGABRT thread signal handlers
  if (signal(SIGSEGV, threadSignalHandler) == SIG_ERR) {
      std::cerr << "Error setting up SIGSEGV signal handler." << std::endl;
      return;
  }
  
  if (signal(SIGABRT, threadSignalHandler) == SIG_ERR) {
      std::cerr << "Error setting up SIGABRT signal handler." << std::endl;
      return;
  }

  try {
    for (unsigned long i = start; i < end; i++) {

      if (exitThreads.load()) {
          return;
      }

      std::vector<std::string> parts = splitString(combinations[i], ',');
      std::string file1 = getStemFromFilePath(parts[0]);
      std::string file2 = getStemFromFilePath(parts[1]);
      cx_byte* template1 = probeData[file1];
      cx_byte* template2;
      if (galleryData != nullptr){
        template2 = (*galleryData)[file2];
      } else {
        template2 = probeData[file2];  
      }

      // Calculate the similarity
      cx_real similarity = CX_MatchFRtemplates(*p_aureus, template1, templateSize, template2, templateSize, msg);
      if (similarity == -1){
        std::cout << "[THREAD " << threadNumber << " ERROR] Could not compare " << file1 << " and " << file2 << std::endl;
      }

      std::lock_guard<std::mutex> lock(matchingMutex);
      simMat[file1][file2] = similarity;

      int current = i - start + 1;
      if (current % modulus == 0 || i == end - 1) {
        int range = end - start;
        std::cout << "[THREAD " << threadNumber << "]: " << current << " / " << range << std::endl;
      }
    }
  }
  catch (const std::exception& e) {
      std::cout << "\nAn exception occurred in a thread:\n" << e.what() << std::endl;
      quitThreads.store(true);
  }
  catch (...) {
      std::cout << "\nAn unknown exception occurred in a thread" << std::endl;
      quitThreads.store(true);
  }
}

int main(int argc, char* argv[]){
  cxxopts::Options options("cx", "CyberExtruder 6.1.5 | Feature Matcher");

  options.add_options()
    ("n,group_name", "name of the group or comparison (default: group)", cxxopts::value<std::string>()->default_value("group"))
    ("g,gallery_dir", "path to the gallery directory", cxxopts::value<std::string>())
    ("p,probe_dir", "path to the probe directory", cxxopts::value<std::string>())
    ("o,output_dir", "path to the output directory", cxxopts::value<std::string>())
    ("score_file_type", "save as csv or npy (default: csv)", cxxopts::value<std::string>()->default_value("csv"))
    // ("matrix_file_type", "save as csv or npy (default: none)", cxxopts::value<std::string>()->default_value("none"))
    ("t,threads", "number of threads", cxxopts::value<int>()->default_value("-1")) // Default to -1 for auto
    ("h,help", "Print help");

  auto result = options.parse(argc, argv);

  if (result.count("help")) {
      std::cout << options.help() << std::endl;
      return 0;
  }

  if (!result.count("output_dir") || !result.count("probe_dir")) {
      std::cerr << "Error: Missing required options." << std::endl;
      std::cout << options.help() << std::endl;
      return 1;
  }

  std::string scoreType = result["score_file_type"].as<std::string>();
  if (scoreType != "csv" && scoreType != "npy"){
    std:cerr << "[ERROR] Score File Type can only be csv or npy." << std::endl;
    std::cout << options.help() << std::endl;
    return 1;
  }

  std::string groupName = result["group_name"].as<std::string>();
  std::filesystem::path outputDirectory = result["output_dir"].as<std::string>();
  std::filesystem::path probeDirectory = result["probe_dir"].as<std::string>();
  std::filesystem::path galleryDirectory = probeDirectory;
  if (result.count("gallery_dir")){
    galleryDirectory = result["gallery_dir"].as<std::string>();
  }

  // std::string matrixType = result["matrix_file_type"].as<std::string>();
  // if (matrixType != "csv" && matrixType != "bin" && matrixType != "none"){
  //   std::cerr << "[ERROR] Matrix File Type can only be csv or bin." << std::endl;
  //   std::cout << options.help() << std::endl;
  //   return 1;
  // }

  
  int numThreads = result["threads"].as<int>();

  // If numThreads is set to -1, use std::thread::hardware_concurrency() / 2
  if (numThreads == -1) {
      numThreads = std::thread::hardware_concurrency() / 2;
  }
  
  // Set up the SIGINT and SIGTERM signal handlers
  if (signal(SIGINT, signalHandler) == SIG_ERR) {
    std::cerr << "Error setting up SIGINT signal handler." << std::endl;
    return 1;
  }

  if (signal(SIGTERM, signalHandler) == SIG_ERR) {
    std::cerr << "Error setting up SIGTERM signal handler." << std::endl;
    return 1;
  }
  
  char msg[1024];
  int load_gallery = 0; 
  bool load_FR_engines = true;
  bool use_fr = true;

  if (use_fr){
    load_gallery = 1; // instruct Aureus to load the FR gallery
    load_FR_engines = true; // must also load the engines
  }

  p_aureus = CX_CreateAureus(msg);
  if (!p_aureus){
    printf("Failed to create Aureus Object:\n%s\n", msg);
    return 0;
  }

  printf("Aureus Status: Created\n");

  // Setting Application Name
  strcpy(msg, "Aureus Xavi");
  CX_SetAppInfo(p_aureus, msg);
  // CX_SetInitializationCallBack(p_aureus, Aureus_Init_CallBack, NULL, msg);

  if (!CX_GetVersion(p_aureus, msg)) printf("Failed to get Aureus Version:\n%s\n", msg);
  else printf("Aureus Version = %s\n", msg);

  string g_install_dir;
  GetExecutablePath(g_install_dir);

  // Looking for that file that needs to be there
  string fname = g_install_dir + "/ADL_HNN.data";
  if (!FileExists(fname.c_str())){
    g_install_dir = "./";
  }

  printf("Initializing Aureus from %s\n", g_install_dir.c_str());
  if (CX_Initialize(p_aureus, load_gallery, NULL, g_install_dir.c_str(), msg) == 1){
    int num_streams, image_enabled, fr_enabled, forensic_enabled;
    int enterprise_enabled, maintenance_enabled, sdk_enabled;
    unsigned long long gal_size;

    printf("Successful initialization\n");
    printf("------- License Info --------\n");
    (void)CX_GetLicenseInfo(p_aureus, msg);
    printf("License Info: %s\n", msg);
    (void)CX_GetLicenseParameters(p_aureus, &num_streams, &image_enabled, &fr_enabled, &forensic_enabled, &enterprise_enabled, &maintenance_enabled, &sdk_enabled, &gal_size, msg);
    printf("Number of licensed video streams = %d\n", num_streams);
    printf("Image enabled = %d\n", image_enabled);
    printf("FR Enabled = %d\n", fr_enabled);
    printf("Forensic Enabled = %d\n", forensic_enabled);
    printf("Enterprise Enabled = %d\n", enterprise_enabled);
    printf("Maintenance Enabled = %d\n", maintenance_enabled);
    printf("SDK Enabled = %d\n", sdk_enabled);
    printf("Gallery Size = %d\n", (unsigned int)gal_size);
    printf("-----------------------------\n");

    // Loading FR engines
    if (load_FR_engines){
      int n_engines = CX_GetNumFRengines(p_aureus, msg);
      if (n_engines < 0){
        printf("%s\n", msg);
        freeAureus();
        // CX_FreeAureus(p_aureus, msg);
        return 0;
      } else {
        printf("There are %d FR engines\n", n_engines);
        // Print out the engine names
        for (int i = 0; i < n_engines; i++){
          printf("Engine number %d = %s\n", i, CX_GetFRname(p_aureus, i, msg));
        }
        if (n_engines == 0){
          printf("No FR Engines FR will not be performed!\n");
        } else {
          // we'll select the last engine as this tends to be the best
          int engine_num = n_engines - 1;
          printf("Selecting FR engine: %s\n", CX_GetFRname(p_aureus, engine_num, msg));
          if (CX_SelectFRengine(p_aureus, engine_num, msg) == 0){
            printf("Failed to select engine %d, error = %s\n", engine_num, msg);
            // CX_FreeAureus(p_aureus, msg);
            freeAureus();
            return 0;
          }
          else printf("Sucessfully selected engine %d\n", engine_num);
        }
      }

      // if we are using FR with a gallery make sure it's up to date
      // only reason it would be out of date (apart from corruption) is
      // if we have added a new engine
      if (use_fr){
        // register an FR update callback (will print info during update)
        CX_SetUpdateFRCallBack(p_aureus, FRupdateCallback, NULL, msg);
        // update the FR (this just makes sure all templates are present)
        CX_UpdateFR(p_aureus, msg);
        printf("\n"); 
      }
    }
  } else {
    printf("Failed to Initialize Aureus:\n%s\n", msg);
    return 0;
  }

  CX_DetectionParams fdp;
  fdp.m_top = 0.001;
  fdp.m_left = 0.001;
  fdp.m_height = 0.999;
  fdp.m_width = 0.999;
  fdp.m_min_height_prop = 0.2;
  fdp.m_max_height_prop = 0.8;

  const std::vector<std::string> fileExtensions = {".bin"};
  std::vector<std::string> galleryPaths;
  std::vector<std::string> probePaths = getFilesWithExtensions(probeDirectory, fileExtensions);
  
  std::cout << "[INFO] Probe: " << probeDirectory << std::endl;
  std::cout << "[INFO] Gallery: " << galleryDirectory << std::endl;
  if (probeDirectory != galleryDirectory){
    galleryPaths = getFilesWithExtensions(galleryDirectory, fileExtensions);
  } else {
    galleryPaths = probePaths;
  }
  
  std::cout << "[INFO]: Found " << probePaths.size() << " images in the probe directory." << std::endl;
  std::cout << "[INFO]: Found " << galleryPaths.size() << " images in the gallery directory." << std::endl;

  int templateSize = CX_GetTemplateSize(p_aureus, msg);
  if (templateSize == -1){
    freeAureus();
    return 0;
  }

  std::unordered_map<std::string, cx_byte*> probeData, galleryData;
  std::unordered_map<std::string, cx_byte*>* pGalleryData;
  std::vector<std::string> combinations;
  std::unordered_map<std::string, std::unordered_map<std::string, float>> simMat;

  probeData = readTemplates(probePaths, templateSize);
  if (probeDirectory != galleryDirectory){
    galleryData = readTemplates(galleryPaths, templateSize);
    pGalleryData = &galleryData;
    combinations = generateCombinations(probePaths, galleryPaths);
  } else {
    galleryData = probeData;
    pGalleryData = nullptr;
    combinations = generateCombinations(probePaths);
  }

  std::vector<std::thread> matchingThreads;
  matchingThreads.reserve(numThreads);

  int batchSize = std::ceil(static_cast<double>(combinations.size()) / numThreads);
  int modulus = int(batchSize * 0.1);
  if (modulus < 2) modulus = 1;

  std::cout << "\n[INFO] Threads: " << numThreads << " | Matching Batch Size: " << batchSize << std::endl;

  for (int i = 0; i < numThreads; i++) {
    unsigned long start = i * batchSize;
    unsigned long end = (i == numThreads - 1) ? combinations.size() : (i + 1) * batchSize;
    auto threadFunc = std::bind(processCombination, 
        (i+1), 
        std::ref(combinations), 
        std::ref(probeData), 
        std::ref(simMat), 
        &p_aureus, 
        &fdp, 
        templateSize, 
        msg, 
        modulus, 
        start, 
        end, 
        pGalleryData);
    matchingThreads.emplace_back(threadFunc);
  }

  // Wait for all threads to finish
  for (auto& thread : matchingThreads) {
    thread.join();
  }

  freeAureus();

  if (exitThreads.load()) {
    std::cout << "[INFO] Exiting the program." << std::endl;
    exit(EXIT_SUCCESS); // Terminate the program
  }

  // Open a file for writing
  if (scoreType == "csv"){
    std::filesystem::path authenticFilePath = outputDirectory / (groupName + "_authentic_scores." + scoreType);
    std::filesystem::path impostorFilePath = outputDirectory / (groupName + "_impostor_scores." + scoreType);
    std::ofstream authenticFile(authenticFilePath.string());
    std::ofstream impostorFile(impostorFilePath.string());
    // Check if the file was successfully opened
    if (!authenticFile.is_open()) {
        std::cerr << "[ERROR] Could not open output authentic file." << std::endl;
        return 1;
    }
    if (!impostorFile.is_open()) {
        std::cerr << "[ERROR] Could not open output impostor file." << std::endl;
        return 1;
    }
    
    std::cout << "[INFO] Writing authentic scores to " << authenticFilePath << std::endl;
    std::cout << "[INFO] Writing impostor scores to " << impostorFilePath << std::endl;

    // Writing to impostor and authentic files
    for (const auto& entry1 : simMat) {
        const std::string& file1 = entry1.first;
        std::string subject1 = splitString(file1, '_')[0];
        for (const auto& entry2 : entry1.second) {
            const std::string& file2 = entry2.first;
            std::string subject2 = splitString(file2, '_')[0];
            float similarity = entry2.second;
            if (subject1 == subject2){
              authenticFile << file1 << "," << file2 << "," << similarity << "\n";
            } else {
              impostorFile << file1 << "," << file2 << "," << similarity << "\n";
            }          
        }
    }
      
    // Close the files when done
    authenticFile.close();
    impostorFile.close();

  } else if (scoreType == "npy"){
    std::filesystem::path authenticLabelsFilePath = outputDirectory / (groupName + "_authentic_scores_labels.txt");
    std::filesystem::path impostorLabelsFilePath = outputDirectory / (groupName + "_impostor_scores_labels.txt");
    std::filesystem::path authenticNpyFilePath = outputDirectory / (groupName + "_authentic_scores.npy");
    std::filesystem::path impostorNpyFilePath = outputDirectory / (groupName + "_impostor_scores.npy");
    std::ofstream authenticFile(authenticLabelsFilePath.string());
    std::ofstream impostorFile(impostorLabelsFilePath.string());

    if (!authenticFile.is_open()) {
        std::cerr << "[ERROR] Could not open output authentic labels file." << std::endl;
        return 1;
    }
    if (!impostorFile.is_open()) {
        std::cerr << "[ERROR] Could not open output impostor labels file." << std::endl;
        return 1;
    }
    
    std::cout << "[INFO] Writing authentic labels to " << authenticLabelsFilePath << std::endl;
    std::cout << "[INFO] Writing impostor labels to " << impostorLabelsFilePath << std::endl;

    // Writing to impostor and authentic files
    std::vector<float> authenticScores, impostorScores;
    for (const auto& entry1 : simMat) {
        const std::string& file1 = entry1.first;
        std::string subject1 = splitString(file1, '_')[0];
        for (const auto& entry2 : entry1.second) {
            const std::string& file2 = entry2.first;
            std::string subject2 = splitString(file2, '_')[0];
            float similarity = entry2.second;
            if (subject1 == subject2){
              authenticFile << file1 << " " << file2 << "\n";
              authenticScores.push_back(similarity);
            } else {
              impostorFile << file1 << " " << file2 << "\n";
              impostorScores.push_back(similarity);
            }          
        }
    }
    authenticFile.close();
    impostorFile.close();

    std::cout << "[INFO] Writing authentic scores to " << authenticNpyFilePath << std::endl;
    npy::npy_data_ptr<float> authenticNpy;
    authenticNpy.data_ptr = authenticScores.data();
    authenticNpy.shape = {authenticScores.size()};
    authenticNpy.fortran_order = false;
    npy::write_npy(authenticNpyFilePath, authenticNpy);
    std::cout << "[INFO] Writing impostor scores to " << impostorNpyFilePath << std::endl;
    npy::npy_data_ptr<float> impostorNpy;
    impostorNpy.data_ptr = impostorScores.data();
    impostorNpy.shape = {impostorScores.size()};
    impostorNpy.fortran_order = false;
    npy::write_npy(impostorNpyFilePath, impostorNpy);
  }

  std::cout << "[INFO] Done!" << std::endl;
  return 0;
}