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
- `export AFLCHURN_DISABLE_AGE=1`: disable rdays
- `export AFLCHURN_ENABLE_RANK=rrank`: enable rrank and disable rdays

- `export AFLCHURN_DISABLE_CHURN=1`: disable the number of changes of lines during build processd.

    `export AFLCHURN_CHURN_SIG=change`: change; 

    `export AFLCHURN_CHURN_SIG=change2`: change^2;

    `export AFLCHURN_CHURN_SIG=logchange`: log2(change);

- `export AFLCHURN_INST_RATIO=N`: N%, select N% BBs to be inserted churn/age


Install

    cd /path/to/root/aflchurn
    make clean all
    cd llvm_mode
    make clean all


### About configure
Export environmental variables.
    
    CC=/path/to/aflchurn/afl-clang-fast ./configure [...options...]
    make

Be sure to also include CXX set to afl-clang-fast++ for C++ code.

### configure the time period to record churns

    export AFLCHURN_SINCE_MONTHS=num_months

e.g., `export AFLCHURN_SINCE_MONTHS=6` indicates recording changes in the recent 6 months

## run fuzzing

    afl-fuzz -i <input_dir> -o <out_dir> -p anneal -e -Z -- <file_path> [...parameters...]

### option -p
power schedule. Default: anneal.

    -p none
    -p anneal
    -p average

### option -e
Byte score for mutation. 
If `-e` is set, it will not use the ant colony optimisation for mutation.

### option -Z
If `-Z` is set, use alias method to select the next seed based on churns information.
If `Z` is not set, use the vanilla AFL scheme to select the next seed.

### option -s N
`-s` sets value of `scale_exponent` in `energy_factor = pow(2, scale_exponent * (2 * energy_exponent - 1));`.

`N` should be an integer.


### option -H float
set `fitness_exponent` in

```
fitness * (1 - pow(fitness_exponent, q->times_selected)) 
        + 0.5 * pow(fitness_exponent, q->times_selected);
```
### ACO increase/decrease score
if `A` is set, set it to "increase and decrease" mode.

### DEFAULT
pe3, N/days, mul, 100logchange, Texp0.3
