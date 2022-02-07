# cv
Create Video. Windows command line app for turning photos into a video (slideshow)

Use a Visual Studio VC 64 bit command window to build using m.bat

Usage

    Usage: cv [input] /o:[outputname] /b:[Bitrate] /d:[Delay] /e:[EffectMS] /f:[0xBBGGRR] /w:[Width] /h:[Height] /i:[textfile] /p:[threads] /t:[1-5]
      Create Video from a set of image files
      arguments: [input]  Path with wildcard for input files. e.g. c:\pics\*.jpg
                 -b       Bitrate suggestion. Default is 4,000,000 bps
                 -d       Delay between each image in milliseconds. Default is 1000
                 -e       Milliseconds of transition Effect on enter/exit of a frame. Must be < 0.5 of /d. Default is 200
                 -f       Fill color RGB for portions of video a photo doesn't cover. Default is black 0x000000
                 -g       Disable use of GPU for rendering. By default, GPU will be used if available
                 -h       Height of the video (images are scaled then center-cropped to fit). Default is 1080
                 -i       Input text file with paths on each line. Alternative to using [input]
                 -o       Specifies the output file name. Overwrites existing file.
                 -p       Parallelism 1-16. If your images are small, try more. If out of RAM, try less. Default is 4
                 -r       Recurse into subdirectories looking for more images. Default is false
                 -s       Stats: show detailed performance information
                 -t       Add transitions between frames. Transitions types 1-2. Default none.
                 -w       Width of the video (images are scaled then center-cropped to fit). Default is 1920
      examples:  cv *.jpg /o:video.mp4 /d:500 /h:1920 /w:1080
                 cv *.jpg /o:video.mp4 /b:5000000 /h:512 /w:512
                 cv *.jpg /o:video.mp4 /b:5000000 /h:512 /w:512 /f:0x1300ac
                 cv *.jpg /o:video.mp4 /b:2500000 /h:512 /w:512 /p:1
                 cv *.jpg /o:video.mp4 /b:4000000 /h:2160 /w:3840 /p:16
                 cv d:\pictures\slothrust\*.jpg /o:slothrust.mp4 /d:200
                 cv /t:1 d:\pictures\slothrust\*.jpg /o:slothrust.mp4 /d:200
                 cv /f:0x33aa44 /h:1500 /w:1000 /o:y:\shirt.mp4 d:\shirt\*.jpg /d:490 /p:6 -s -g
                 cv /f:0x33aa44 /h:1500 /w:1000 /o:y:\shirt.mp4 d:\shirt\*.jpg /d:490 /p:16 -s
                 cv /f:0x000000 /h:1080 /w:1920 /o:y:\2020.mp4 d:\zdrive\pics\2020_wow\*.jpg /d:4000 /t:1 /e:300 /p:8 -s
      transitions:   1    Fade from/to black
                     2    Fade from/to white
