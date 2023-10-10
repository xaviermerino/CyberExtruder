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

std::mutex simMatMutex;

void Aureus_Init_CallBack(cx_real percent, const char* info, void* p_object){
  printf("%f%% %s\n", percent, info);
}

void FRupdateCallback(const char* msg, cx_int total, cx_int current, void* p_object){
  if (total > 0) printf("Updating FR %s %d/%d   \r", msg, current, total);
}

void LoadImageDirectory(const char* dir, std::vector<CX_RAM_Image>& images){
  bool print_details = true;
  LoadFrames(dir, images, print_details);
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

std::vector<std::string> generateCombinations(const std::vector<std::string> &elements) {
  std::vector<std::string> combinations;
  long n = elements.size();
  combinations.reserve((n * (n-1)) / 2);
  for (int i = 0; i < n; ++i) {
    for (int j = i + 1; j < n; ++j) {
      combinations.push_back(elements[i] + "," + elements[j]);
    }
  }
  return combinations;
}

std::vector<std::string> getFilesWithExtension(const std::string& directoryPath, const std::string& extension) {
  std::vector<std::string> matchingFiles;

  try {
      for (const auto& entry : std::filesystem::recursive_directory_iterator(directoryPath)) {
          if (std::filesystem::is_regular_file(entry) && entry.path().extension() == extension) {
              matchingFiles.push_back(entry.path().string());
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

std::unordered_map<std::string, std::vector<CX_RAM_Image*>> getGalleryMapFromImagesInDirectory(const std::string& directoryPath, const std::string& extension){
  std::unordered_map<std::string, std::vector<CX_RAM_Image*>> data;
  std::vector<std::string> matchingFiles = getFilesWithExtension(directoryPath, extension);
  for (const std::string& path : matchingFiles){
    std::string fileName = getStemFromFilePath(path);
    std::vector<std::string> parts = splitString(fileName, '_');
    std::string subject = parts[0];
    CX_RAM_Image* image = new CX_RAM_Image();
    if (LoadImageFromDisk(path.c_str(), *image)){
      std::cout << "Processed: " << path << std::endl;
      data[subject].push_back(image);
    } else {
      std::cout << "Failed to Process: " << path << std::endl;
    }
  }
  return data;
}

void processCombination(
  int threadNumber,
  const std::vector<std::string>& combinations,
  std::unordered_map<std::string, CX_RAM_Image>& data,
  std::unordered_map<std::string, std::unordered_map<std::string, float>>& simMat,
  CX_AUREUS* p_aureus,
  CX_DetectionParams* fdp,
  char* msg,
  int modulus,
  unsigned long start,
  unsigned long end) {
    
  for (unsigned long i = start; i < end; i++) {
    std::vector<std::string> parts = splitString(combinations[i], ',');
    std::string file1 = getStemFromFilePath(parts[0]);
    std::string file2 = getStemFromFilePath(parts[1]);
    CX_RAM_Image* image1 = &data[file1];
    CX_RAM_Image* image2 = &data[file2];

    // Calculate the similarity
    float similarity = CX_MatchImages(*p_aureus, image1, *fdp, image2, *fdp, msg);

    // Lock the mutex to protect concurrent access to simMat
    std::lock_guard<std::mutex> lock(simMatMutex);

    // Update simMat
    simMat[file1][file2] = similarity;

    int current = i - start + 1;
    if (current % modulus == 0 || i == end - 1) {
      int range = end - start;
      std::cout << "[THREAD " << threadNumber << "]: " << current << " / " << range << std::endl;
    }
  }
}

int main(int argc, char* argv[]){
  char msg[1024];
  int load_gallery = 0; 
  bool load_FR_engines = true;
  bool use_fr = true;

  if (use_fr){
    load_gallery = 1; // instruct Aureus to load the FR gallery
    load_FR_engines = true; // must also load the engines
  }
 
  CX_AUREUS p_aureus = CX_CreateAureus(msg);
  if (!p_aureus){
    printf("Failed to create Aureus Object:\n%s\n", msg);
    return 0;
  }

  printf("Aureus Status: Created\n");

  // Setting Application Name
  strcpy(msg, "Aureus Xavi");
  CX_SetAppInfo(p_aureus, msg);
  CX_SetInitializationCallBack(p_aureus, Aureus_Init_CallBack, NULL, msg);

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
  if (CX_Initialize(p_aureus, load_gallery, "CF", g_install_dir.c_str(), msg) == 1){
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
        CX_FreeAureus(p_aureus, msg);
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
            CX_FreeAureus(p_aureus, msg);
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
  }

  CX_DetectionParams fdp;
  fdp.m_top = 0.001;
  fdp.m_left = 0.001;
  fdp.m_height = 0.999;
  fdp.m_width = 0.999;
  fdp.m_min_height_prop = 0.2;
  fdp.m_max_height_prop = 0.8;

  const std::string directory = "/workspaces/CyberExtruder/sample";
  const std::string fileExtension = ".jpg";
  const std::vector<std::string>& paths = getFilesWithExtension(directory, fileExtension);
  // std::vector<CX_RAM_Image> images;
  // std::vector<CX_RankItem> ranks;
  // images.reserve(paths.size());
  // ranks.reserve(paths.size());

  // // Batch Enroll of Gallery
  // std::cout << "[INFO] Enrolling Gallery" << std::endl;
  // int n_type = 2; // SUBJECT_ABC.jpg
  // int deduplicate = 1; // Aureus will use FR to assign incoming images to people already in the gallery if they already exist.
  // int result = CX_PerformBatchEnroll(p_aureus, n_type, deduplicate, directory.c_str(), msg);
  // if (result == -1){
  //   std::cout << "[ERROR] " << msg;
  // }
  // std::cout << "[INFO] Done Enrolling Gallery!" << std::endl;
  // std::cout << "Files: " << paths.size() << std::endl;
  // for (const std::string& filePath : paths) {
  //   CX_RAM_Image image;
  //   CX_RankItem rank;
  //   if (LoadImageFromDisk(filePath.c_str(), image)){
  //     int result = CX_ApplyFR(p_aureus, &image, &rank, 40, 1, msg);
  //     if (result == -1){
  //       std::cout << "[FR ERROR] " << msg;
  //     } else {
  //       std::cout << "Processed: " << filePath << std::endl;
  //       ranks.push_back(rank);
  //     }
  //     std::cout << "Rank: " << rank.m_score << std::endl;
  //   } else {
  //     std::cout << "Failed to Process: " << filePath << std::endl;
  //   }
  // }

  // for (int i = 0; i < paths.size(); i++){
  //   std::vector<std::string> parts = splitString(getStemFromFilePath(paths[i]), '_');
  //   std::string subject = parts[0];
  //   cout << "Person: " << subject << " | PID: " << ranks[i].m_person_id << " | Name: " << ranks[i].m_person_name << " | Score: " << ranks[i].m_score << std::endl;
  // }
  


  // // My own version of batch enroll.
  // // const std::unordered_map<std::string, std::vector<CX_RAM_Image*>>& data = getGalleryMapFromImagesInDirectory(directory, fileExtension);
  // // for (const auto& entry1 : data){
  // //   const std::string& subject = entry1.first;
  // //   std::vector<CX_RAM_Image*> images = entry1.second;
  // //   std::cout << "Adding to Gallery: " << subject << " | " << images.size() << " images" << std::endl;
  // //   int result = CX_AddNewPersonMultiImage(p_aureus, subject.c_str(), images.data(), images.size(), NULL, NULL, msg);
  // //   if (result == -1){
  // //     cout << "ERROR: " << msg << endl;
  // //   }
  // // }




  // // INFO: Prints out Subject and Number of Images
  // // for (const auto& entry1 : data) {
  // //   const std::string& file1 = entry1.first;
  // //   const int imagesPerSubject = entry1.second.size();
  // //   std::cout << "Subject: " << file1 << " | Images: " << imagesPerSubject << std::endl;
  // // }

  std::unordered_map<std::string, CX_RAM_Image> data;
  // Image-by-Image Matching - SLOW
  for (const std::string& filePath : paths) {
    CX_RAM_Image image;
    if (LoadImageFromDisk(filePath.c_str(), image)){
      std::cout << "Loaded: " << filePath << std::endl;
      data[getStemFromFilePath(filePath)] = image;
    } else {
      std::cout << "Failed to Process: " << filePath << std::endl;
    }
  }

  std::unordered_map<std::string, std::unordered_map<std::string, float>> simMat;
  const std::vector<std::string>& combinations = generateCombinations(paths);
  
  int numThreads = std::thread::hardware_concurrency(); // Get the number of available threads
  std::vector<std::thread> threads;
  threads.reserve(numThreads);

  unsigned long batchSize = std::ceil(static_cast<double>(combinations.size()) / numThreads);
  std::cout << "Threads: " << numThreads << " | Batch Size: " << batchSize << std::endl;

  int modulus;
  if (batchSize > 1000){
    modulus = 100;
  } else if (batchSize > 100){
    modulus = 10;
  } else if (batchSize > 20){
    modulus = 5;
  } else {
    modulus = 1;
  }

  for (int i = 0; i < numThreads; i++) {
    unsigned long start = i * batchSize;
    unsigned long end = (i == numThreads - 1) ? combinations.size() : (i + 1) * batchSize;
    auto threadFunc = std::bind(processCombination, (i+1), std::ref(combinations), std::ref(data), std::ref(simMat), &p_aureus, &fdp, msg, modulus, start, end);
    threads.emplace_back(threadFunc);
  }

  // Wait for all threads to finish
  for (auto& thread : threads) {
    thread.join();
  }

  // for (unsigned long i = 0; i < combinations.size(); i++){
  //   std::vector<std::string> parts = splitString(combinations[i], ',');
  //   std::string file1 = getStemFromFilePath(parts[0]);
  //   std::string file2 = getStemFromFilePath(parts[1]);
  //   CX_RAM_Image* image1 = &data[file1];
  //   CX_RAM_Image* image2 = &data[file2];
  //   simMat[file1][file2] = CX_MatchImages(p_aureus, image1, fdp, image2, fdp, msg);
  //   if (i % modulus == 0 || i == combinations.size() - 1){
  //     cout << "Progress: " << (i+1) << " / " << combinations.size() << std::endl;
  //   }
  // }

  // Open a file for writing
  std::ofstream outputFile("output.csv");

  // Check if the file was successfully opened
  if (!outputFile.is_open()) {
      std::cerr << "Error: Could not open output file." << std::endl;
      return 1; // Return an error code
  }

  std::cout << "Writing results to output.csv" << std::endl;
  // Iterate over the simMat map and write its contents to the file
  for (const auto& entry1 : simMat) {
      const std::string& file1 = entry1.first;
      for (const auto& entry2 : entry1.second) {
          const std::string& file2 = entry2.first;
          float similarity = entry2.second;
          outputFile << file1 << "," << file2 << "," << similarity << std::endl;
      }
  }

  // Close the file when done
  outputFile.close();

  // // Free Aureus
  printf("Freeing Aureus\n");
  if (!CX_FreeAureus(p_aureus, msg)) printf("Failed to free Aureus:\n%s\n", msg);
  else printf("Successfully freed Aureus\n");
  return 0;
}