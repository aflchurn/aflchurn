# source
Based on [american fuzzy lop](https://github.com/google/AFL), which is originally developed by Michal Zalewski <lcamtuf@google.com>.

# this project

fuzzing with change-burst info.

## version
Linux 18.04, 64-bit system. 

LLVM 7.0.1


## Install

   
### install aflchurn
We have two schemes of burst, one is the age of lines and the other is the number of changes of lines. 
We can choose one of the schemes or both of them.
- `export BURST_COMMAND_AGE=1`: enable rdays
- `export BURST_AGE_SIGNAL=rrank`: enable rrank and disable rdays

- `export BURST_COMMAND_CHURN=1` enables the number of changes of lines during build processd.
- `export BURST_CHURN_SIGNAL=...` select the signal of churn. default: change
    ```
    BURST_CHURN_SIGNAL=xlogchange
                        logchange
    ```



Install

    cd /path/to/root/aflchurn
    make clean all
    cd llvm_mode
    make clean all


### About configure
Export environmental variables.
    
    export BURST_COMMAND_CHURN=1
    export BURST_COMMAND_AGE=1
    CC=/path/to/aflchurn/afl-clang-fast ./configure [...options...]
    make

Be sure to also include CXX set to afl-clang-fast++ for C++ code.

### configure the time period to record churns

    export BURST_SINCE_MONTHS=num_months

e.g., `export BURST_SINCE_MONTHS=6` indicates recording changes in the recent 6 months

## run fuzzing

    afl-fuzz -i <input_dir> -o <out_dir> -p anneal -e -Z -- <file_path> [...parameters...]

### option -p
power schedule. Default: anneal.

    -p none
    -p anneal
    -p average

### option -b
Choose "age" or "churn". Default: both

    -b none
    -b age
    -b churn

### option -e
Byte score for mutation. 
If `-e` is set, it will not use the ant colony optimisation for mutation.

### option -Z
If `-Z` is set, use alias method to select the next seed based on churns information.
If `Z` is not set, use the vanilla AFL scheme to select the next seed.

### option -c N
if `-c` is set, N is scale_exponent in `energy_factor = pow(2, scale_exponent * (energy_exponent - normalizing_constant));`


        
### option -G

ADD or MULTIPLY in score_pow = (rela_p_age * rela_p_churn) *(...)
calculation of power schedule:

    -G add
        mul

### option -H


### DEFAULT
pe3, N/days, mul, 100logchange
