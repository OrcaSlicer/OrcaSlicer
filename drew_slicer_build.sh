
outfile="LOG_slicer_`date +%Y_%m_%d_%H_%M_%S`.log"
options="$@"

echo "[drew_slicer_build.sh] Building slicer with options ${options}. Logging output to ${outfile}"

# Sending stdout and stderror to a file as well as stdout with help from https://stackoverflow.com/questions/418896/how-to-redirect-output-to-a-file-and-stdout
a=$(time `./build_linux.sh ${options} 2>&1 | tee ${outfile}`)
# or, for an all in one, you can run: time `./build_linux.sh -Csi 2>&1 | tee LOG_slicer_$(date +%Y_%m_%d_%H_%M_%S).log`
# or time `./build_linux.sh -Csi >> LOG_slicer_$(date +%Y_%m_%d_%H_%M_%S).log`

# To remove config, run rm -rf /var/home/dwingfield/Ubuntu24_04_Orca_Home/.config/OrcaSlicer

echo --------
echo $a
echo Build complete.
