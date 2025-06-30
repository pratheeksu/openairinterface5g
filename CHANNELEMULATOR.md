<script type="text/javascript" async src="https://cdnjs.cloudflare.com/ajax/libs/mathjax/3.2.2/es5/tex-mml-chtml.min.js">
</script>
<script type="text/x-mathjax-config">
 MathJax.Hub.Config({
 tex2jax: {
 inlineMath: [['$', '$'] ],
 displayMath: [ ['$$','$$'], ["\\[","\\]"] ]
 }
 });
</script>

# Overview
This document serves as a guide to loading a pre-generated dataset of Channel Impulse Responses (CIRs)—for example, computed using Sionna RT—and performing real-time 5G emulation using OpenAirInterface (OAI).

For full system architecture details, please refer to: [https://arxiv.org/abs/2503.12177v2](https://arxiv.org/abs/2503.12177v2)

As a sample scenario, this repository includes the Shibuya Scramble Crossing CIR dataset used in the paper, placed under `oai_cir/cir/output/binary/`.

# Supported Environment

|||
|-|-|
|CPU                    | AMD Ryzen 9 7900X 12-core processor (x24) |
|RAM                    | 32 GB                                     |
|Operating System       | Ubuntu 22.04.4 LTS                        |
|Linux kernel version   | 6.8.1-060801-generic                      |
|SDR                    | USRP B200                                 |
|COTS UE                | Quectel RM500Q-GL                         |
|||

# Prerequisites (after setting up base OAI environment)

## 1. Install AOCL-BLAS

```bash
# Install dependencies
sudo apt install build-essential make g++ gfortran cmake

# Build and install BLIS
git clone https://github.com/amd/blis.git
cd blis
./configure --enable-cblas -t openmp auto
make
make check
sudo make install
sudo ldconfig
```

## 2. Configure pkg-config

```bash
# Copy the pkg-config file
sudo cp /usr/local/share/pkgconfig/blis-mt.pc /usr/local/share/pkgconfig/cblis.pc
```

## 3. Configure AOCL-BLAS multithreading

```bash
echo "export BLIS_NUM_THREADS=6" >> ~/.bashrc
source ~/.bashrc
```

For more details, see:
[https://github.com/amd/blis/blob/master/docs/Multithreading.md](https://github.com/amd/blis/blob/master/docs/Multithreading.md)

---

## Configuration: `oai_cir/cir_conf.txt`

The 2nd through 5th rows of the parameters in the `oai_cir/cir_conf.txt` as shown in below can be set freely (the 1st row is read-only).

1. **Max path gain \[dB]** in the CIR dataset (read-only).
2. **Offset \[dB]** added to compensate signal attenuation (typically around (–2)× the first-line value + 5), to fit within the gNB/UE receiver's dynamic range.
3. **Noise level \[dB]**.
4. **Number of delay taps** $N$ to apply in the convolution.
5. **Number of CIR data files** to vary over time based on the mobility scenario.

Notes:

* Line 2: To maintain a continuous connection between the gNB and UE throughout the sample scenario, this **Offset** parameter must be finely tuned. If the Offset is set too low, the UE either fails to attach to the gNB or detaches almost immediately. If it is set too high, a COTS UE’s automatic-gain control will engage, complicating the link-budget calculation; with an OAI UE or RF-simulation-based nrUE, the signal will exceed the dynamic range and the UE will likewise fail to attach. In the test environment, a value of roughly **(Max path gain × –2) + 5 dB** seemed to be appropriate.

* Line 4: After bandwidth filtering, delay taps are sorted by descending amplitude, and only the top $N$ taps are used.
  With the sample CIR dataset, you can specify any value from 1 to 146.
  In the test environment, real-time communication between gNB (USRP B200) and COTS UE (Quectel RM500Q-GL) was possible up to $N = 29$; for larger values the real-time processing could not keep up.

* Line 5: Specifies how many files (maximum = number of files in `oai_cir/cir/output/binary/`, default 230; minimum = 1) to load in sequence.
  Each file's CIR data is convolved with the baseband signal for 100 ms, then the emulator switches to the next file. 
  Once the last file finishes, the cycle restarts at `delay{amp/index}list0000.b`.

### Binary file formats

#### `delayamplist****.b`
$h^{\rm re}_{\rm sort,1}, h^{\rm im}_{\rm sort,1}, h^{\rm re}_{\rm sort,2}, h^{\rm im}_{\rm sort,2}, \cdots$

Each $h^{\rm re/im}_{{\rm sort},n}$ is the real/imaginary part (float32) of the $n$-th highest-amplitude complex tap from the sorted CIR.

#### `delayindexlist****.b`
$i_{\rm sort,1}, i_{\rm sort,2}, i_{\rm sort,3}, \cdots$

Each $i_{{\rm sort},n}$ is the original delay index (int32) before sorting.

---

# Build and Run Emulator

## Option 1: gNB with USRP
Build gNB with USRP mode.
```bash
cd ~/oai_cir/cmake_targets/
sudo ./build_oai -w USRP --gNB
```

Run gNB with USRP mode.
```bash
cd ~/oai_cir/cmake_targets/ran_build/build
./nr-softmodem \
  -O ../../../targets/PROJECTS/GENERIC-NR-5GC/CONF/gnb.sa.band77.fr1.106PRB.usrpb210.chemu.yaml \
  --sa --non-stop -E
```

## Option 2: RF Simulator with gNB and nrUE
Build gNB and nrUE with RF simulation mode.
```bash
cd ~/oai_cir/cmake_targets/
sudo ./build_oai -w SIMU --gNB --nrUE
```

Run gNB with RF simulation mode.
```bash
cd ~/oai_cir/cmake_targets/ran_build/build
sudo ./nr-softmodem \
  -O ../../../targets/PROJECTS/GENERIC-NR-5GC/CONF/gnb.sa.band77.fr1.106PRB.usrpb210.chemu.yaml \
  --sa --non-stop --rfsim --rfsimulator.serveraddr server
```

Run nrUE with RF simulation mode.
```bash
cd ~/oai_cir/cmake_targets/ran_build/build
sudo ./nr-uesoftmodem \
  -C 4019160000 -r 106 --numerology 1 --ssb 144 \
  --rfsim --non-stop --band 77 --rfsimulator.serveraddr 127.0.0.1
```

# Citation
If you use this implementation, please cite it as:
```bibtex
@misc{OWDT,
    title = {Open Wireless Digital Twin: End-to-End 5G Mobility Emulation in O-RAN Framework},
    author = {Tetsuya Iye and Masaya Sakamoto and Shohei Takaya and Eisaku Sato and Yuki Susukida and Yu Nagaoka and Kazuki Maruta and Jin Nakazato},
    year = {2025},
    eprint={2503.12177},
    archivePrefix={arXiv},
    primaryClass={cs.NI},
    url={https://arxiv.org/abs/2503.12177v2}, 
}
```