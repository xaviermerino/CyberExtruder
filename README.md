# CyberExtruder Face Matcher

This repository contains code for utilizing the Aureus3D SDK by CyberExtruder for face matching. It enables the extraction of facial templates from a gallery directory and calculates authentic and impostor matching scores.

## ⚠️ Pre-Requisites 

>**Important Note:** This repository does not include proprietary Aureus code and requires you to have obtained both the SDK files and an Aureus license. Without these prerequisites, the code in this repository will not function correctly.

Ensure that the `Aureus_Tracking` directory provided in this repository is placed within the main `Aureus` SDK directory. Here is an example directory structure for the `Aureus` directory:

```bash
.
├── AASDK.data
├── ACD.events
├── ADL_HNN.data
├── AES_Log.txt
├── AGSDK.data
├── algc_b03c304e3254.data
├── Aureus
├── Aureus_Tracking
├── Aureus_Tracking_exe
├── AureusVideoGUI
├── AureusVideoGUI_exe
├── cec.data
├── DwellTime.data
├── FNNS.data
├── FR
├── InstallGUIlibs.sh
├── IsFace.data
├── jpeg
├── LandmarksFNNX.data
├── libarm
├── libAureus.so
├── liblinux
├── libtbb.so
├── libtbb.so.2
├── lic.txt
├── MOPC.data
├── POST
├── restart_app.sh
├── Results
├── SDK_Log.txt
├── SequentialFrames
├── TestImages
├── UbuntuReadMe.txt
├── Videos
└── wxWidgets-3.0.4
```

## 🛠️ Building the Image 
> **Note:** If you would rather pull the image directly, visit this [page](https://github.com/xaviermerino/CyberExtruder-Private). It will take you to a private repository with instructions on how to pull the image, provided you have been granted prior access. Once pulled you may skip this section and keep following the guide.

Assuming you have placed the `Aureus_Tracking` directory within the `Aureus` SDK directory as mentioned above, you can build the image using the provided `Dockerfile`. Navigate to the root of the repository and execute the following command:

```bash
docker build -t cx:6.1.5 .
```

The build process will compile the application and place it in the correct directories. Once completed, the image will be labeled as `cx:6.1.5`.

## 🐳 Running the Image
After successfully building the image, you can proceed to deploy a container for execution. The container requires the mounting of volumes for the following information:

- Location of the license file
- Location of the data to be processed
- Location to save the output

For example, you can run a container with the following command:

```bash
docker run --name cx --rm \
-v /home/xavier/Documents/git/CyberExtruder/lic.txt:/Aureus/lic.txt \
-v /home/xavier/Documents/git/CyberExtruder/sample:/data \
-v /home/xavier/Documents:/output \
cx:6.1.5 \
--output /output \
--name sample \
--directory /data
```

Here's what each volume mount does:

- `-v /home/xavier/Documents/git/CyberExtruder/lic.txt:/Aureus/lic.txt` mounts your license inside the container. Do not modify the container's license file location (`/Aureus/lic.txt`).

- `-v /home/xavier/Documents/git/CyberExtruder/sample:/data` mounts the gallery directory inside the container at `/data`. You can adjust the container's target directory as needed.

> **Note:** Files within the `/data` directory must follow the format `<subject>_<anything>.jpg`, where `<subject>` is used for mate and non-mate comparisons. The `<anything>` part can contain additional details. The application can process `.jpg` and `.JPG` files only.

- `-v /home/xavier/Documents:/output` mounts your local environment's output directory inside the container at `/output`. All results will be saved in this directory. You can adjust the container's target directory as needed.

Additionally, you need to specify options for the application. Here are the available options:

| Option          | Description                                    | Suggested Value | Required |
|-----------------|------------------------------------------------|-----------------|----------|
| --output, -o    | Output directory for match score files         | /output         | Yes      |
| --name, -n      | Group name or output file name prefix          | N/A             | Yes      |
| --directory, -d | Gallery directory for processing               | /data           | Yes      |
| --threads, -t   | Number of threads for processing (-1 for auto) | -1              | No       |

## 📉 Interpreting Results

### 📄 Files Generated
After running the application, you will notice several generated files. An application message will indicate the file names and their locations. Here is an example:

```
[INFO] Successfully freed Aureus!
[INFO] Writing authentic scores to "/output/sample_authentic.csv"
[INFO] Writing impostor scores to "/output/sample_impostor.csv"
[INFO] Writing enrollment summary to "/output/summary.txt"
[INFO] Done!
```

If some images fail to enroll, you will obtain an additional file detailing the failed images:

```
[INFO] Successfully freed Aureus!
[INFO] Writing authentic scores to "/output/sample_authentic.csv"
[INFO] Writing impostor scores to "/output/sample_impostor.csv"
[INFO] Writing enrollment summary to "/output/summary.txt"
[INFO] Writing enrollment failure summary to "/output/missing.txt"
[INFO] Done!
```

### 💯 Authentic and Impostor Scores Files
Below is an excerpt of how the authentic and impostor `.csv` files look like. The first two columns indicate the files being compared, and the third column provides the match score. Scores range between 0 and 1, with values over 0.75 typically considered verified match scores.

```
087899_13F45,076422_05F51,0.395052
087899_13F45,105458_03F35,0.433416
082272_13F43,076422_05F51,0.997153
082272_13F43,087899_13F45,0.395052
082272_13F43,097706_01F43,0.633953
082272_13F43,092803_01F39,0.499069
082272_13F43,105458_03F35,0.538921
082272_13F43,104821_06F39,0.641931
092803_01F39,076422_05F51,0.499069
092803_01F39,105458_03F35,0.556945
```

### ✅ Summary Files
The `summary.txt` and `missing.txt` files provide an overview of which files were processed and which ones failed. They list file paths processed from the container's perspective. For example:

```
/data/095453_07F37.jpg
/data/098718_10F37.jpg
/data/087970_00F44.jpg
/data/063379_05F43.jpg
/data/080505_01F41.jpg
/data/093682_00F53.jpg
/data/076678_04F44.jpg
/data/098718_00F34.jpg
/data/079391_02F45.jpg
/data/087026_02F41.jpg
```

## ⛔ Possible Errors

1. **Number of Threads:** It is recommended to let the application automatically select the number of threads. If you specify a specific number of threads and the program exits abruptly, it may not free Aureus properly.

2. **SIGINT Signal:** If the container receives a SIGINT signal, it will exit abruptly without freeing Aureus.

3. **Out of Processing Streams:** If the program exits abruptly due to a signal or thread issue, repeatedly running the program with the same options may exhaust your license's available Aureus streams. These streams will become available again after a certain amount of time.