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
#include <algorithm>
#include <execution>
#include <random>
#include <array>


#define NUM_BATCHES 64

CX_AUREUS p_aureus;
std::mutex templateMutex;
std::mutex failToEnrollMutex;
std::vector<std::string> paths;
std::vector<std::string> fte;

// Atomic variable threads check to see if they need to exit
std::atomic<bool> stopRequested(false);
std::atomic<size_t> activeThreads(0); 
std::unordered_map<unsigned int, std::unordered_map<unsigned int, float>> simMat;

void freeAureus() {
  char msg[1024];
  if (!CX_FreeAureus(p_aureus, msg)) {
      printf("Failed to free Aureus:\n%s\n", msg);
  } else {
      printf("[INFO] Successfully freed Aureus!\n");
  }
}


void cleanupAndExit(int signum) {
  stopRequested.store(true); // Request stopping of threads
  auto startTime = std::chrono::system_clock::now();
  while (activeThreads.load() > 0 && 
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now() - startTime).count() < 5) 
  { 
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  freeAureus();
  
  exit(signum);
}

void setupSignalHandlers() {
  signal(SIGINT, cleanupAndExit);
  signal(SIGTERM, cleanupAndExit);
  signal(SIGSEGV, cleanupAndExit);  
}

// Function to handle thread-specific signals (SIGSEGV and SIGABRT)
void threadSignalHandler(int signal) {
  stopRequested.store(true);
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

std::string getStemFromFilePath(const std::string path) {
  std::filesystem::path filePath(path);
  std::string fileNameWithoutExtension = filePath.stem().string();
  return fileNameWithoutExtension;
}

void generateTemplates(
  const std::filesystem::path& templateDirectory,
  CX_AUREUS* p_aureus,
  CX_DetectionParams* fdp,
  int templateSize) {
  
  char msg[1024];
  std::cout << "[INFO]: Generating templates.\n";
  std::atomic<size_t> atomicIndex(0);
  std::atomic<size_t> completedTasks(0);
  std::atomic<bool> isGeneratingDone(false);

  auto reportProgress = [&]() {
    while (!isGeneratingDone.load() && !stopRequested.load()) {
      size_t currentCompleted = completedTasks.load();
      double percentage = static_cast<double>(currentCompleted) / paths.size() * 100;
      std::cout << "\rProgress: " << percentage << "% completed.";
      std::this_thread::sleep_for(std::chrono::seconds(1));
      std::cout << std::flush;
    }
    if (!stopRequested.load()){
      std::cout << "\rProgress: " << 100 << "% completed.";
      std::cout << std::flush;
    }
  };

  std::thread progressReporter(reportProgress);

  std::vector <std::array<unsigned long, 2>> batchEndpoints;
  batchEndpoints.reserve(NUM_BATCHES);
  int numFiles = paths.size();
  unsigned long batchSize = std::ceil(static_cast<double>(numFiles) / NUM_BATCHES);

  for (int i = 0; i < NUM_BATCHES; i++) {
    std::array<unsigned long, 2> endpoints;
    unsigned long start = i * batchSize;
    unsigned long end = (i == NUM_BATCHES - 1) ? numFiles : (i + 1) * batchSize;
    endpoints[0] = start;
    endpoints[1] = end;
    batchEndpoints.push_back(endpoints);
  }

  std::for_each(std::execution::par, batchEndpoints.begin(), batchEndpoints.end(), [&](const std::array<unsigned long, 2>& endpoints){
    activeThreads.fetch_add(1);

    size_t i = atomicIndex.fetch_add(1);
    for (int i = endpoints[0]; i < endpoints[1]; i++) {
      if (stopRequested.load()) {
        return;
      }

      std::string filePath = paths[i];
      std::filesystem::path templateFileName = templateDirectory / (getStemFromFilePath(filePath) + ".bin");
      CX_RAM_Image image;
      cx_byte* pTemplate = new cx_byte[templateSize];
      if (LoadImageFromDisk(filePath.c_str(), image)){
        int result = CX_GenerateTemplate(*p_aureus, &image, *fdp, pTemplate, msg);
        if (result == 1){
          std::ofstream file(templateFileName.c_str(), std::ios::out | std::ios::binary);
          if (file.is_open()) {
            file.write(reinterpret_cast<const char*>(pTemplate), templateSize);
            file.close();
          } else {
            std::cerr << "Could not open " << templateFileName << " for writing." << std::endl;
          }
        } else {
          std::cout << "FTE: " << filePath << std::endl;
          std::lock_guard<std::mutex> lock(failToEnrollMutex);
          fte.push_back(filePath);
        }
      } else {
        std::cerr << "[ERROR] Failed to Load: " << filePath << std::endl;
      }
      CX_Free_RAM_Image(&image, msg);
      completedTasks.fetch_add(1);
    }
    activeThreads.fetch_sub(1);
  });

  isGeneratingDone.store(true);
  progressReporter.join();
  std::cout << "\n[INFO]: Template generation completed." << std::endl;
}


int main(int argc, char* argv[]){
  cxxopts::Options options("cx", "CyberExtruder 6.1.5 | Template Generator");

  options.add_options()
    ("o,output", "Directory where to store templates", cxxopts::value<std::string>())
    ("d,directory", "Gallery directory", cxxopts::value<std::string>())
    // ("t,threads", "Number of threads", cxxopts::value<int>()->default_value("-1")) // Default to -1 for auto
    ("h,help", "Print help");

  auto result = options.parse(argc, argv);

  if (result.count("help")) {
    std::cout << options.help() << std::endl;
    return 0;
  }

  if (!result.count("output") || !result.count("directory")) {
    std::cerr << "Error: Missing required options." << std::endl;
    std::cout << options.help() << std::endl;
    return 1;
  }

  std::string gallery = result["directory"].as<std::string>();
  std::filesystem::path directory = result["output"].as<std::string>();
  std::filesystem::path missingFilename = "missing.txt";
  std::filesystem::path completedFilename = "summary.txt";

  std::filesystem::path templatePath = directory / "templates";
  std::filesystem::path summaryPath = directory / "summary";
  if (!std::filesystem::exists(templatePath)) {
    if (!std::filesystem::create_directory(templatePath)) {
      std::cout << "[ERROR] Failed to create templates directory." << std::endl;
      return -1;
    }
  }
  if (!std::filesystem::exists(summaryPath)) {
    if (!std::filesystem::create_directory(summaryPath)) {
      std::cout << "[ERROR] Failed to create summary directory." << std::endl;
      return -1;
    }
  }
  std::filesystem::path missingPath = summaryPath / missingFilename;
  std::filesystem::path completedPath = summaryPath / completedFilename;

  setupSignalHandlers();
  
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

  if (!CX_GetVersion(p_aureus, msg)) printf("Failed to get Aureus Version:\n%s\n", msg);
  else printf("Aureus Version = %s\n", msg);

  string g_install_dir;
  GetExecutablePath(g_install_dir);

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

      if (use_fr){
        CX_SetUpdateFRCallBack(p_aureus, FRupdateCallback, NULL, msg);
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

  const std::vector<std::string> fileExtensions = {".jpg", ".jpeg", ".JPG", ".JPEG"};
  paths = getFilesWithExtensions(gallery, fileExtensions);
  int numberFiles = paths.size();
  std::cout << "Found " << numberFiles << " images." << std::endl;

  int templateSize = CX_GetTemplateSize(p_aureus, msg);
  if (templateSize == -1){
    freeAureus();
    return 0;
  }

  std::cout << "[INFO] Template Generation Started!" << std::endl;
  generateTemplates(std::ref(templatePath), &p_aureus, &fdp, templateSize);

  std::cout << "[INFO] Template Generation Done!" << std::endl;
  if (fte.size() > 0){
    std::cout << "[INFO] Failed to enroll " << fte.size() << " images." << std::endl;
    // Sort both vectors to use std::set_difference
    std::sort(fte.begin(), fte.end());
    std::sort(paths.begin(), paths.end());
    std::vector<std::string> result;
    // Keep only filepaths that are not in FTE vector
    std::set_difference(paths.begin(), paths.end(), fte.begin(), fte.end(), std::inserter(result, result.begin()));
    paths = result;
  }

  int numberEnrolled = paths.size();
  std::cout << "[INFO] Enrolled " << numberEnrolled << " / " << numberFiles << " images." << std::endl;
  std::cout << "\n[INFO] Template Matching Started! " << std::endl;

  freeAureus();
  std::ofstream completedFile(completedPath.string());
  if (!completedFile.is_open()) {
    std::cerr << "Error: Could not open output enrollment summary file." << std::endl;
    return 1;
  }

  std::cout << "[INFO] Writing enrollment summary to " << completedPath << std::endl;
  for (const std::string& filepath : paths){
    completedFile << getStemFromFilePath(filepath) << "\n";
  }
  completedFile.close();

  if (fte.size() > 0){
    std::ofstream missingFile(missingPath.string());
    if (!missingFile.is_open()) {
      std::cerr << "[ERROR] Could not open output enrollment failure file." << std::endl;
    } else {
      std::cout << "[INFO] Writing enrollment failure summary to " << missingPath << std::endl;
      for (const std::string& filepath : fte){
        missingFile << getStemFromFilePath(filepath) << "\n";
      }
      missingFile.close();
    }
  }

  std::cout << "[INFO] Done!" << std::endl;
  return 0;
}