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
#include "npy.hpp"

CX_AUREUS p_aureus;
std::mutex matchingMutex;
std::vector<std::string> galleryPaths;
std::vector<std::string> probePaths;
std::vector<cx_byte*> probeData, galleryData;
std::atomic<bool> stopRequested(false);
std::atomic<size_t> activeThreads(0); 
int templateSize;
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

std::vector<std::pair<unsigned int, unsigned int>> generateCombinations(const std::vector<std::string> &probePaths) {
  std::vector<std::pair<unsigned int, unsigned int>> combinations;
  long n = probePaths.size();
  combinations.reserve((n * (n-1)) / 2);
  for (int i = 0; i < n; ++i) {
    for (int j = i + 1; j < n; ++j) {
      combinations.push_back(std::pair<unsigned int, unsigned int>(i, j));
    }
  }
  return combinations;
}

std::string getStemFromFilePath(const std::string path) {
  std::filesystem::path filePath(path);
  std::string fileNameWithoutExtension = filePath.stem().string();
  return fileNameWithoutExtension;
}

std::vector<std::pair<unsigned int, unsigned int>> generateCombinations(const std::vector<std::string>& probePaths, const std::vector<std::string>& galleryPaths) {
  std::vector<std::pair<unsigned int, unsigned int>> combinations;
  combinations.reserve(probePaths.size() * galleryPaths.size());
  for (unsigned int i = 0; i < probePaths.size(); ++i) {
    std::string probeImage = getStemFromFilePath(probePaths[i]);
    for (unsigned int j = 0; j < galleryPaths.size(); ++j) {
      std::string galleryImage = getStemFromFilePath(galleryPaths[j]);

      // This portion makes sure that the same instance of a subject is removed.
      // For example, if the gallery has a "ABC_123" and the probes have "ABC_123_cloaked"
      // then it will not be considered
      if (probeImage.find(galleryImage) == std::string::npos){ 
        combinations.push_back(std::pair<unsigned int, unsigned int>(i, j));
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

std::vector<cx_byte*> readTemplates(const std::vector<std::string>& paths, int templateSize) {
  std::vector<cx_byte*> data(paths.size()); // Preallocate with the size of paths
  std::for_each(std::execution::par, paths.begin(), paths.end(), [&](const std::string& path) {
      std::ifstream file(path.c_str(), std::ios::in | std::ios::binary);
      if (file.is_open()) {
          cx_byte* pTemplate = new cx_byte[templateSize];
          file.read(reinterpret_cast<char*>(pTemplate), templateSize);
          file.close();
          
          // Calculate the index for insertion
          size_t index = &path - &paths[0];
          data[index] = pTemplate;
      }
  });
  return data;
}

void processProbeGalleryMatch(){
  char msg[1024];
  std::cout << "[INFO]: Matching probe and gallery templates." << std::endl;
  std::atomic<size_t> atomicIndex(0);
  std::atomic<size_t> completedTasks(0);
  std::atomic<bool> isProcessingDone(false);

  auto reportProgress = [&]() {
    while (!isProcessingDone.load() && !stopRequested.load()) {
      size_t currentCompleted = completedTasks.load();
      double percentage = static_cast<double>(currentCompleted) / probePaths.size() * 100;
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

  std::for_each(std::execution::par, probePaths.begin(), probePaths.end(), [&](const std::string& probePath) {
    activeThreads.fetch_add(1);

    size_t i = atomicIndex.fetch_add(1);
    const std::string& probeImage = getStemFromFilePath(probePath);
    for (size_t j = 0; j < galleryPaths.size(); ++j) {
      if (stopRequested.load()) {
        return; 
      }

      const std::string& galleryImage = getStemFromFilePath(galleryPaths[j]);
      if (probeImage.find(galleryImage) == std::string::npos) {
        cx_byte* template1 = probeData[i];
        cx_byte* template2 = galleryData[j];
        cx_real similarity = CX_MatchFRtemplates(p_aureus, template1, templateSize, template2, templateSize, msg);
        if (similarity != -1){
          std::lock_guard<std::mutex> lock(matchingMutex);
          simMat[i][j] = similarity;
        }
      }
    }
    completedTasks.fetch_add(1);
    activeThreads.fetch_sub(1);
  });

  isProcessingDone.store(true);
  progressReporter.join();
  std::cout << "\n[INFO]: Matching completed." << std::endl;
}

void processProbeOnlyMatch(){
  char msg[1024];
  std::cout << "[INFO]: Matching probe templates." << std::endl;
  std::atomic<size_t> atomicIndex(0);
  std::atomic<size_t> completedTasks(0);
  std::atomic<bool> isProcessingDone(false);  

  auto reportProgress = [&]() {
    while (!isProcessingDone.load() && !stopRequested.load()) {
      size_t currentCompleted = completedTasks.load();
      double percentage = static_cast<double>(currentCompleted) / probePaths.size() * 100;
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

  std::for_each(std::execution::par, probePaths.begin(), probePaths.end(), [&](const std::string& probePath) {
    size_t i = atomicIndex.fetch_add(1);
    const std::string& probeImage = getStemFromFilePath(probePaths[i]);
    for (size_t j = i + 1; j < probePaths.size(); ++j){
      if (stopRequested.load()) {
        return; 
      }

      cx_byte* template1 = probeData[i];
      cx_byte* template2 = probeData[j];
      cx_real similarity = CX_MatchFRtemplates(p_aureus, template1, templateSize, template2, templateSize, msg);

      if (similarity != -1){
        std::lock_guard<std::mutex> lock(matchingMutex);
        simMat[i][j] = similarity;
      }

    }
    completedTasks.fetch_add(1);
    activeThreads.fetch_sub(1);
  });

  isProcessingDone.store(true);
  progressReporter.join();
  std::cout << "\n[INFO]: Matching completed." << std::endl;
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
    // ("t,threads", "number of threads", cxxopts::value<int>()->default_value("-1")) // Default to -1 for auto
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

  const std::vector<std::string> fileExtensions = {".bin"};
  probePaths = getFilesWithExtensions(probeDirectory, fileExtensions);
  
  std::cout << "[INFO] Probe: " << probeDirectory << std::endl;
  std::cout << "[INFO] Gallery: " << galleryDirectory << std::endl;
  if (probeDirectory != galleryDirectory){
    galleryPaths = getFilesWithExtensions(galleryDirectory, fileExtensions);
  } else {
    galleryPaths = probePaths;
  }
  
  std::cout << "[INFO]: Found " << probePaths.size() << " images in the probe directory." << std::endl;
  std::cout << "[INFO]: Found " << galleryPaths.size() << " images in the gallery directory." << std::endl;

  templateSize = CX_GetTemplateSize(p_aureus, msg);
  // templateSize = 128;
  if (templateSize == -1){
    freeAureus();
    return 0;
  }

  std::vector<cx_byte*>* pGalleryData;
  std::vector<std::pair<unsigned int, unsigned int>> combinations;

  std::cout << "[INFO]: Reading probe templates." << std::endl;
  probeData = readTemplates(probePaths, templateSize);
  if (probeDirectory != galleryDirectory){
    std::cout << "[INFO]: Reading gallery templates." << std::endl;
    galleryData = readTemplates(galleryPaths, templateSize);
    pGalleryData = &galleryData;
    processProbeGalleryMatch();
  } else {
    galleryData = probeData;
    pGalleryData = nullptr;
    processProbeOnlyMatch();
  }

  freeAureus();

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
      const unsigned int firstIndex = entry1.first;
      std::string file1 = getStemFromFilePath(probePaths[firstIndex]);
      std::string subject1 = splitString(file1, '_')[0];
      for (const auto& entry2 : entry1.second) {
        const unsigned int secondIndex = entry2.first;
        std::string file2;
        if (pGalleryData != nullptr){
          file2 = galleryPaths[secondIndex];
        } else {
          file2 = probePaths[secondIndex];
        }
        file2 = getStemFromFilePath(file2);
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
      const unsigned int firstIndex = entry1.first;
      std::string file1 = getStemFromFilePath(probePaths[firstIndex]);
      std::string subject1 = splitString(file1, '_')[0];
      for (const auto& entry2 : entry1.second) {
        const unsigned int secondIndex = entry2.first;
        std::string file2;
        if (pGalleryData != nullptr){
          file2 = galleryPaths[secondIndex];
        } else {
          file2 = probePaths[secondIndex];
        }
        file2 = getStemFromFilePath(file2);
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