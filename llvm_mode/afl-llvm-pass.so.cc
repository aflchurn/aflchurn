/*
  Copyright 2015 Google LLC All rights reserved.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at:

    http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
*/

/*
   american fuzzy lop - LLVM-mode instrumentation pass
   ---------------------------------------------------

   Written by Laszlo Szekeres <lszekeres@google.com> and
              Michal Zalewski <lcamtuf@google.com>

   LLVM integration design comes from Laszlo Szekeres. C bits copied-and-pasted
   from afl-as.c are Michal's fault.

   This library is plugged into LLVM when invoking clang through afl-clang-fast.
   It tells the compiler to add code roughly equivalent to the bits discussed
   in ../afl-as.h.
*/

#define AFL_LLVM_PASS

#include "../config.h"
#include "../debug.h"

//#include <string.h>
#include <set>
#include <map>
#include <cmath>


#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <iostream>
#include <fstream>
#include <string>
#include <sstream>
#include <list>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "llvm/ADT/Statistic.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Debug.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"

#include "llvm/Support/CommandLine.h"
#include "llvm/IR/DebugLoc.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/DebugInfo.h"



using namespace llvm;


namespace {

  class AFLCoverage : public ModulePass {

    public:

      static char ID;

      AFLCoverage() : ModulePass(ID) { }

      bool runOnModule(Module &M) override;

      // StringRef getPassName() const override {
      //  return "American Fuzzy Lop Instrumentation";
      // }
      

  };

}


char AFLCoverage::ID = 0;


bool startsWith(std::string big_str, std::string small_str){
  if (big_str.compare(0, small_str.length(), small_str) == 0) return true;
  else return false;
}

double inst_norm_age(int max_days, int days_since_last_change){
  double norm_days;
  // if (days_since_last_change < 0) norm_days = 1;
  // else norm_days = 
  //     1 / (log2(days_since_last_change + 2) * log2(days_since_last_change + 2));

  /* Normalize 1/days */
  if (days_since_last_change <= 0 || max_days <= 1) {
    norm_days = 1;
    WARNF("Current days are less than 0 or maximum days are less than 1.");
  }
  else{
    norm_days = (double)(max_days - days_since_last_change) / 
                            (days_since_last_change * (max_days - 1));
  }

  return norm_days;

}

double inst_norm_rank(unsigned int max_rank, int line_rank){
  double norm_ranks;

  /* rrank */
  if (line_rank <= 0) {
    norm_ranks = 1;
    WARNF("Rank of lines is less than 0.");
  }
  else norm_ranks = 1 / (double)line_rank;

  return norm_ranks;

}

double inst_norm_change(unsigned int num_changes, unsigned short change_select){
  double norm_chg = 0;

  switch(change_select){
    case CHURN_LOG_CHANGE:
      // logchanges
      if (num_changes < 0) norm_chg = 0;
      else norm_chg = log2(num_changes + 1);
      break;

    case CHURN_CHANGE:
      norm_chg = num_changes;
      break;

    case CHURN_CHANGE2:
      // change^2
      norm_chg = (double)num_changes * num_changes;
      break;
    default:
      FATAL("Wrong CHURN_CHANGE type!");
  }

  return norm_chg;

}


/* use popen() to execute git command */
std::string execute_git_cmd (std::string directory, std::string str_cmd){
  FILE *fp;
  int rc=0;
  std::string str_res = "";
  char ch_git_res[2048];
  std::ostringstream git_cmd;
  git_cmd << "cd " 
          << directory
          << " && "
          << str_cmd;
  fp = popen(git_cmd.str().c_str(), "r");
	if(NULL == fp) return str_res;
	// when cmd fail, output "fatal: ...";
  // when succeed, output result
  if (fscanf(fp, "%s", ch_git_res) == 1) {
    str_res.assign(ch_git_res);
  }

  if (startsWith(str_res, "fatal")){
    str_res = "";
  }

  rc = pclose(fp);
  if(-1 == rc){
    printf("git command pclose() fails\n");
  }

  return str_res;

}

/* Get the number of changes that is used to choose between "always instrument"
        or "insturment randomly".
Note: #changes of a file is always larger than #changes of a line in the file,
    so THRESHOLD_PERCENT_CHANGES is set in a low value. */
int get_threshold_changes(std::string directory){

  std::ostringstream changecmd;
  unsigned int largest_changes = 0;
  int change_threshold = 0;
  FILE *dfp;
  // The maximum number of changes to any file.
  changecmd << "cd " << directory
          << " && git log --name-only --pretty=\"format:\""
          << " | sed '/^\\s*$/d' | sort | uniq -c | sort -n"
          << " | tr -s ' ' | sed \"s/^ //g\" | cut -d\" \" -f1 | tail -n1";
  dfp = popen(changecmd.str().c_str(), "r");
  if(NULL == dfp) return WRONG_VALUE;

  if (fscanf(dfp, "%u", &largest_changes) != 1) return WRONG_VALUE;

  change_threshold = (THRESHOLD_PERCENT_CHANGES * largest_changes) / 100;

  pclose(dfp);

  return change_threshold;
}

/* if return value is 0, something wrong happens */
unsigned long get_commit_time_days(std::string directory, std::string git_cmd){
  unsigned long unix_time = 0;
  FILE *dfp;
  std::ostringstream datecmd;

  datecmd << "cd " << directory
          << " && "
          << git_cmd;
  dfp = popen(datecmd.str().c_str(), "r");

  if (NULL == dfp) return WRONG_VALUE;
  if (fscanf(dfp, "%lu", &unix_time) != 1) return WRONG_VALUE;
  pclose(dfp);

  return unix_time / 86400;

}

/* Get the number of commits before HEAD;
  if return value is 0, something wrong happens */
unsigned int get_max_ranks(std::string git_directory){

  FILE *dfp;
  unsigned int head_num_parents;
  std::ostringstream headcmd;
  headcmd << "cd " << git_directory
          << " && git rev-list --count HEAD";
  dfp = popen(headcmd.str().c_str(), "r");
  if (NULL == dfp) return WRONG_VALUE;
  if (fscanf(dfp, "%u", &head_num_parents) != 1) return WRONG_VALUE;
  pclose(dfp);

  return head_num_parents;

}

/* 
 git diff current_commit HEAD -- file_path. 
 Help get the related lines in HEAD commits, which are related to the lines from git show.
 */
void git_diff_current_head(std::string cur_commit_sha, std::string git_directory, 
            std::string relative_file_path, std::set<unsigned int> &changed_lines_from_show,
                std::map <unsigned int, unsigned int> &lines2changes){

    std::ostringstream cmd;
    char array_head_changes[32], array_current_changes[32], fatat[12], tatat[12];
    std::string current_line_range, head_line_result;
    size_t cur_comma_pos, head_comma_pos;
    int rc = 0;
    FILE *fp;
    int cur_line_num, cur_num_start, cur_num_count, head_line_num, head_num_start, head_num_count;
    std::set<unsigned int> cur_changed_lines, head_changed_lines;
    bool is_head_changed = false, cur_head_has_diff = false;

    /* git diff -U0 cur_commit HEAD -- filename | grep ...
      get the changed line range between current commit and HEAD commit;
      help get the changed lines in HEAD commits;
      result: "@@ -8,0 +9,2 @@"
            (-): current commit; (+): HEAD commit
    */
    cmd << "cd " << git_directory << " && git diff -U0 " << cur_commit_sha << " HEAD -- " << relative_file_path
        << " | grep -o -P \"^@@ -[0-9]+(,[0-9])? \\+[0-9]+(,[0-9])? @@\"";

    fp = popen(cmd.str().c_str(), "r");
    if(NULL == fp) return;
    /* -: current_commit;
       +: HEAD */
    // result: "@@ -8,0 +9,2 @@" or "@@ -10 +11,0 @@" or "@@ -466,8 +475 @@" or "@@ -8 +9 @@"
    while(fscanf(fp, "%s %s %s %s", fatat, array_current_changes, array_head_changes, tatat) == 4){

        cur_head_has_diff = true;
        
        current_line_range.clear(); /* The current commit side, (-) */
        current_line_range.assign(array_current_changes); // "-"
        current_line_range.erase(0,1); //remove "-"
        cur_comma_pos = current_line_range.find(",");
        /* If the changed lines in current commit can be found in changed_lines_from_show, 
            the related lines in HEAD commit should count for changes. */
        if (cur_comma_pos == std::string::npos){
            cur_line_num = std::stoi(current_line_range);
            if (changed_lines_from_show.count(cur_line_num)) is_head_changed = true;
        }else{
            cur_num_start = std::stoi(current_line_range.substr(0, cur_comma_pos));
            cur_num_count = std::stoi(current_line_range.substr(cur_comma_pos + 1, 
                                                                current_line_range.length() - cur_comma_pos - 1));
            for(int i=0; i< cur_num_count; i++){
                if (changed_lines_from_show.count(cur_num_start + i)){
                    is_head_changed = true;
                    break;
                }
            }
        }

        /* Trace changes for head commit, increment lines2changes.
          Some lines are changed in current commit, so trace these lines back to HEAD commit,
          and increment the count of these lines in HEAD commit. 
          */
        if (is_head_changed){
            head_line_result.clear(); /* The head commit side, (+) */
            head_line_result.assign(array_head_changes); // "+"
            head_line_result.erase(0,1); //remove "+"
            head_comma_pos = head_line_result.find(",");

            if (head_comma_pos == std::string::npos){
                head_line_num = std::stoi(head_line_result);
                if (lines2changes.count(head_line_num)) lines2changes[head_line_num]++;
                else lines2changes[head_line_num] = 1;
            }else{
                head_num_start = std::stoi(head_line_result.substr(0, head_comma_pos));
                head_num_count = std::stoi(head_line_result.substr(head_comma_pos + 1, 
                                                                  head_line_result.length() - head_comma_pos - 1));
                for(int i=0; i< head_num_count; i++){ 
                    if (lines2changes.count(head_num_start + i)) lines2changes[head_num_start + i]++;
                    else lines2changes[head_num_start + i] = 1; 
                }
            }

        }

        memset(array_current_changes, 0, sizeof(array_current_changes));
        memset(array_head_changes, 0, sizeof(array_head_changes));
    }

    /* if there's no diff in current commit and HEAD commit;
     there's no change of the file between two commits;
     so any change in current commit (compared to its parents) counts for the HEAD commit*/

    if (!cur_head_has_diff){
        for (auto mit = changed_lines_from_show.begin(); mit != changed_lines_from_show.end(); ++mit){
            if (lines2changes.count(*mit)) lines2changes[*mit]++;
            else lines2changes[*mit] = 1;
        }
    }

    rc = pclose(fp);
    if(-1 == rc){
        printf("git diff pclose() fails\n");
    }
}

/* git show, get changed lines in current commit.
    It'll show you the log message for the commit, and the diff of that particular commit.
    Find the changed line numbers in file relative_file_path as it was changed in commit cur_commit_sha, 
    and add them to the list changed_lines_cur_commit     
 */
void git_show_current_changes(std::string cur_commit_sha, std::string git_directory, 
            std::string relative_file_path, std::set<unsigned int> &changed_lines_cur_commit){

    std::ostringstream cmd;
    
    char array_parent_changes[32], array_current_changes[32], fatat[12], tatat[12];
    std::string current_line_range;
    size_t comma_pos;
    int rc = 0;
    FILE *fp;
    int line_num, num_start, num_count; 

    // git show: parent_commit(-) current_commit(+)
    // result: "@@ -8,0 +9,2 @@" or "@@ -10 +11,0 @@" or "@@ -466,8 +475 @@" or "@@ -8 +9 @@"
    cmd << "cd " << git_directory << " && git show --oneline -U0 " << cur_commit_sha << " -- " << relative_file_path
          << " | grep -o -P \"^@@ -[0-9]+(,[0-9])? \\+[0-9]+(,[0-9])? @@\"";

    fp = popen(cmd.str().c_str(), "r");
    if(NULL == fp) return;
    // get numbers in (+): current commit
    
    while(fscanf(fp, "%s %s %s %s", fatat, array_parent_changes, array_current_changes, tatat) == 4){

      current_line_range.clear(); /* The current commit side, (+) */
      current_line_range.assign(array_current_changes); // "+"
      current_line_range.erase(0,1); //remove "+"
      comma_pos = current_line_range.find(",");

      if (comma_pos == std::string::npos){
          line_num = std::stoi(current_line_range);
          if (line_num >= 0) changed_lines_cur_commit.insert(line_num);
      }else{
          num_start = std::stoi(current_line_range.substr(0, comma_pos));
          num_count = std::stoi(current_line_range.substr(comma_pos+1, 
                                                          current_line_range.length() - comma_pos - 1));
          for(int i = 0; i< num_count; i++){
              if (num_start >= 0) changed_lines_cur_commit.insert(num_start + i);
          }
      }
      memset(array_current_changes, 0, sizeof(array_current_changes));
    }

    rc = pclose(fp);
    if(-1 == rc){
        printf("git show pclose() fails\n");
    }
}

/* use git command to get line changes */
void calculate_line_change_git_cmd(std::string relative_file_path, std::string git_directory,
                    std::map<std::string, std::map<unsigned int, double>> &file2line2change_map,
                    unsigned short change_sig){
    
  std::ostringstream cmd;
  std::string str_cur_commit_sha;
  char ch_cur_commit_sha[128];
  int rc = 0;
  FILE *fp;
  std::set<unsigned int> changed_lines_cur_commit;
  std::map <unsigned int, unsigned int> lines2changes;
  std::map <unsigned int, double> tmp_line2changes;
  
  // get the commits that change the file of relative_file_path
  // result: commit short SHAs
  // TODO: If the file name changed, it cannot get the changed lines.
  //  --since=10.years 
  char* ch_month = getenv("AFLCHURN_SINCE_MONTHS");
  if (ch_month){
    std::string since_month(ch_month);
    if (since_month.find_first_not_of("0123456789") == std::string::npos){ // all digits
      
      cmd << "cd " << git_directory 
          << " && git log --since=" << ch_month << ".months"
          << " --follow --oneline --format=\"%h\" -- " 
          << relative_file_path << " | grep -Po \"^[0-9a-f]*$\"";
          
    } else {
      ch_month = NULL; // if env variable is not digits, use all commits
    }
  }
  
  if (!ch_month){

    cmd << "cd " << git_directory 
        << " && git log" 
        <<" --follow --oneline --format=\"%h\" -- " 
        << relative_file_path
        << " | grep -Po \"^[0-9a-f]*$\""; 
  }
  
  fp = popen(cmd.str().c_str(), "r");
  if(NULL == fp) return;
  /* get lines2changes: git log -> git show -> git diff
    "git log -- filename": get commits SHAs changing the file
    "git show $commit_sha -- filename": get changed lines in current commit
    "git diff $commit_sha HEAD -- filename": get the related lines in HEAD commit
    */
  while(fscanf(fp, "%s", ch_cur_commit_sha) == 1){
      str_cur_commit_sha.clear();
      str_cur_commit_sha.assign(ch_cur_commit_sha);
      // get changed_lines_cur_commit: the change lines in current commit
      changed_lines_cur_commit.clear();
      git_show_current_changes(str_cur_commit_sha, git_directory, 
                                  relative_file_path, changed_lines_cur_commit);
      // get lines2changes: related change lines in HEAD commit
      git_diff_current_head(str_cur_commit_sha, git_directory, relative_file_path, 
                              changed_lines_cur_commit, lines2changes);
      
  }

  /* Get changes */
  if (!lines2changes.empty()){
    // logchanges
    for (auto l2c : lines2changes){
      tmp_line2changes[l2c.first] = inst_norm_change(l2c.second, change_sig);
    }

    file2line2change_map[relative_file_path] = tmp_line2changes;
    
  }
  
  rc = pclose(fp);
  if(-1 == rc){
      printf("git log pclose() fails\n");
  }

}


/* get age of lines using git command line. 
  git_directory: /home/usrname/repo/
*/
bool calculate_line_age_git_cmd(std::string relative_file_path, std::string git_directory,
                    std::map<std::string, std::map<unsigned int, double>> &file2line2age_map,
                    unsigned long head_commit_days, unsigned long init_commit_days){

  std::map<unsigned int, double> line_age_days;

  /*
  getting pairs [unix_time line_number]
  // cd ${git_directory} &&
  // git blame --date=unix ${relative_file_path} \
  // | grep -o -P "[0-9]{9}[0-9]? [0-9]+"
  */

  std::ostringstream cmd;
  int rc = 0;
  FILE *fp;
  unsigned long unix_time;
  unsigned int line;
  int days_since_last_change;

  if (head_commit_days==WRONG_VALUE || init_commit_days==WRONG_VALUE) return false;

  int max_days = head_commit_days - init_commit_days;

  cmd << "cd " << git_directory << " && git blame --date=unix " << relative_file_path
        << " | grep -o -P \"[0-9]{9}[0-9]? +[0-9]+\"";

  fp = popen(cmd.str().c_str(), "r");
  if(NULL == fp) return false;
  // get line by line
  while(fscanf(fp, "%lu %u", &unix_time, &line) == 2){
    days_since_last_change = head_commit_days - unix_time / 86400; //days

    line_age_days[line] = inst_norm_age(max_days, days_since_last_change);
    
  }

  if (!line_age_days.empty())
      file2line2age_map[relative_file_path] = line_age_days;

  rc = pclose(fp);
  if(-1 == rc){
    printf("git blame pclose() fails\n");
  }

  return true;

}

/* get rank of line ages.
  rank = (the number of commits until HEAD) - (the number of commits until commit A);
 */
bool cal_line_age_rank(std::string relative_file_path, std::string git_directory,
                std::map<std::string, std::map<unsigned int, double>> &file2line2rank_map,
                std::map<std::string, double> &commit2rank,
                unsigned int head_num_parents){

  char line_commit_sha[256];
  FILE *dfp, *curdfp;
  unsigned int nothing_line, line_num;
  std::map<unsigned int, double> line_rank;
  unsigned int cur_num_parents;
  int rank4line;

  if (head_num_parents == WRONG_VALUE) return false;

  /* output: commit_hash old_line_num current_line_num
        e.g., 9f1a353f68d6586b898c47c71a7631cdc816215f 167 346
   */
  std::ostringstream blamecmd;
  blamecmd << "cd " << git_directory
        << " && git blame -p -- " << relative_file_path
        << " | grep -o \"^[0-9a-f]* [0-9]* [0-9]*\"";

  dfp = popen(blamecmd.str().c_str(), "r");
  if(NULL == dfp) return false;

  std::ostringstream rankcmd;
  while (fscanf (dfp, "%s %u %u", line_commit_sha, &nothing_line, &line_num) == 3){
    std::string str_cmt(line_commit_sha);
    if (commit2rank.count(str_cmt)){
      line_rank[line_num] = commit2rank[str_cmt];
    } else {
      rankcmd.str("");
      rankcmd.clear();
      rankcmd << "cd " << git_directory
              << " && git rev-list --count "
              << line_commit_sha;
      curdfp = popen(rankcmd.str().c_str(), "r");
      if(NULL == curdfp) continue;
      if (fscanf (curdfp, "%u", &cur_num_parents) == 1){
        rank4line = head_num_parents - cur_num_parents;
        commit2rank[str_cmt] = line_rank[line_num] 
                             = inst_norm_rank(head_num_parents, rank4line);
      }
      pclose(curdfp);
    }
    
  }
  pclose(dfp);

  if (!line_rank.empty()) file2line2rank_map[relative_file_path] = line_rank;
  return true;
  
}



/* Check if file exists in HEAD using command mode.
return:
    exist: 1; not exist: 0 */
bool is_file_exist(std::string relative_file_path, std::string git_directory){

  //string cmd("cd /home/usrname/repo && git cat-file -e HEAD:util/read.h 2>&1");
  std::ostringstream cmd;

  char result_buf[1024];
  int rc = 0;
  bool isSuccess = false;
  FILE *fp;

  if(access(git_directory.c_str(), F_OK) == -1) return false;
  
  cmd << "cd " << git_directory << " && git cat-file -e HEAD:" 
      << relative_file_path << " 2>&1";

	fp = popen(cmd.str().c_str(), "r");
	if(NULL == fp) return false;
	// when cmd fail, output "fatal: Path 'tdio.h' does not exist in 'HEAD'";
  // when succeed, output nothing
  if (fgets(result_buf, sizeof(result_buf), fp) != NULL) isSuccess = false;
  else isSuccess = true;
	
  rc = pclose(fp);
  if(-1 == rc){
    printf("git cat-file pclose() fails\n");
  }
  
  return isSuccess;

}


/* Change the filename to relative path (relative to souce dir) without "../" or "./" in the path.
Input:
  relative_file_path: relative path of source files, relative to base_directory
  base_directory: absolute path of directories in building directory
  git_directory: absolute path of git repo directory (root of source code)
Output:
  clean relative path of a file
 */
std::string get_file_path_relative_to_git_dir(std::string relative_file_path, 
                    std::string base_directory, std::string git_directory){

    std::string clean_relative_path;

  
  if (startsWith(relative_file_path, "/")){
    // "/path/to/configure": relative_file_path = /path/to/file.c
    // remove substring, which is the same as git_directory, from relative_file_path
    relative_file_path.erase(0, git_directory.length());  // relative path
    clean_relative_path = relative_file_path;
  } else{
    // "../configure" or "./configure"
    // relative_file_path could be src/file.c, build/../src/file.c, or src/./file.c
    // relative_file_path is relative to base_directory here
    base_directory.append("/");
    base_directory.append(relative_file_path);
    // remove "../" or "./"
    char* resolved_path = realpath(base_directory.c_str(), NULL);
    //TODO: why is it NULL?
    if (resolved_path == NULL) clean_relative_path = "";
    else{
      clean_relative_path.append(resolved_path);

      free(resolved_path);

      clean_relative_path.erase(0, git_directory.length());  // relative path
    }  
  }

  return clean_relative_path;

}




bool AFLCoverage::runOnModule(Module &M) {

  LLVMContext &C = M.getContext();
  
  Type *VoidTy;
  IntegerType *Int1Ty;
  IntegerType *Int8Ty;
  IntegerType *Int32Ty;
  IntegerType *Int64Ty;
  Type *Int8PtrTy;
  Type *Int64PtrTy;
  Type *DoubleTy;
  Type *FloatTy;
  GlobalVariable *AFLMapPtr;
  GlobalVariable *AFLPrevLoc;
  unsigned NoSanMetaId;
  MDTuple *NoneMetaNode;
  VoidTy = Type::getVoidTy(C);
  Int1Ty = IntegerType::getInt1Ty(C);
  Int8Ty = IntegerType::getInt8Ty(C);
  Int32Ty = IntegerType::getInt32Ty(C);
  Int64Ty = IntegerType::getInt64Ty(C);
  Int8PtrTy = PointerType::getUnqual(Int8Ty);
  Int64PtrTy = PointerType::getUnqual(Int64Ty);
  DoubleTy = Type::getDoubleTy(C);
  FloatTy = Type::getFloatTy(C);
  NoSanMetaId = C.getMDKindID("nosanitize");
  NoneMetaNode = MDNode::get(C, None);

  /* Show a banner */

  char be_quiet = 0;

  if (isatty(2) && !getenv("AFL_QUIET")) {

    SAYF(cCYA "afl-llvm-pass " cBRI VERSION cRST " by <aflchurn>\n");

  } else be_quiet = 1;

  /* Decide instrumentation ratio */

  char* inst_ratio_str = getenv("AFL_INST_RATIO");
  unsigned int inst_ratio = 100;

  if (inst_ratio_str) {

    if (sscanf(inst_ratio_str, "%u", &inst_ratio) != 1 || !inst_ratio ||
        inst_ratio > 100)
      FATAL("Bad value of AFL_INST_RATIO (must be between 1 and 100)");

  }

  // default: instrument changes and days
  bool use_cmd_change = true, use_cmd_age_rank = false, use_cmd_age = true;

  char *day_sig_str, *change_sig_str;

  unsigned short change_sig = CHURN_LOG_CHANGE; //day_sig, 

  if (getenv("AFLCHURN_DISABLE_AGE")) use_cmd_age = false;
  day_sig_str = getenv("AFLCHURN_ENABLE_RANK");
  if (day_sig_str){
    if (!strcmp(day_sig_str, "rrank")){
      use_cmd_age_rank = true;
      use_cmd_age = false;
    } else{
      FATAL("Set proper age signal");
    }
  }

  if (getenv("AFLCHURN_DISABLE_CHURN")) use_cmd_change = false;
  change_sig_str = getenv("AFLCHURN_CHURN_SIG");
  if (change_sig_str){
    if (!use_cmd_change) FATAL("Cannot simultaneously set AFLCHURN_DISABLE_CHURN and AFLCHURN_CHURN_SIG!");
    if (!strcmp(change_sig_str, "logchange")){
      change_sig = CHURN_LOG_CHANGE;
    } else if (!strcmp(change_sig_str, "change")){
      change_sig = CHURN_CHANGE;
    } else if (!strcmp(change_sig_str, "change2")){
      change_sig = CHURN_CHANGE2;
    } else {
      FATAL("Wrong change signal.");
    }
  }

  unsigned int bb_select_ratio = CHURN_INSERT_RATIO;
  char *bb_select_ratio_str = getenv("AFLCHURN_INST_RATIO");

  if (bb_select_ratio_str) {

    if (sscanf(bb_select_ratio_str, "%u", &bb_select_ratio) != 1 || !bb_select_ratio ||
        bb_select_ratio > 100)
      FATAL("Bad value of AFLCHURN_INST_RATIO (must be between 1 and 100)");

  }

  /* Get globals for the SHM region and the previous location. Note that
     __afl_prev_loc is thread-local. */

  AFLMapPtr =
      new GlobalVariable(M, PointerType::get(Int8Ty, 0), false,
                         GlobalValue::ExternalLinkage, 0, "__afl_area_ptr");

  AFLPrevLoc = new GlobalVariable(
      M, Int32Ty, false, GlobalValue::ExternalLinkage, 0, "__afl_prev_loc",
      0, GlobalVariable::GeneralDynamicTLSModel, 0, false);

  /* Instrument all the things! */

  int inst_blocks = 0, inst_ages = 0, inst_changes = 0, inst_fitness = 0;
  double module_total_ages = 0, module_total_changes = 0, module_total_fitness = 0,
      module_ave_ages = 0, module_ave_chanegs = 0, module_ave_fitness = 0;

  // Choose part of BBs to insert the age/change signal
  int changes_inst_threshold = 0; // for change
  unsigned long init_commit_days = 0, head_commit_days = 0; // for age
  unsigned int head_num_parents = 0; // for ranks
  double norm_change_thd = 0, norm_age_thd = 0, norm_rank_thd = 0;

  std::set<unsigned int> bb_lines;
  std::set<std::string> unexist_files, processed_files;
  unsigned int line;
  std::string git_path;
  
  int git_no_found = 1, // 0: found; otherwise, not found
      is_one_commit = 0; // don't calculate for --depth 1

  std::map<std::string, double> commit_rank;
  // file name (relative path): line NO. , score
  std::map<std::string, std::map<unsigned int, double>> map_age_scores, map_bursts_scores, map_rank_age;

  for (auto &F : M){
    /* Get repository path and object */
    if (git_no_found && !is_one_commit){
      SmallVector<std::pair<unsigned, MDNode *>, 4> MDs;
      std::string funcdir, funcfile, func_abs_path;// func_clean_path;
      
      F.getAllMetadata(MDs);
      for (auto &MD : MDs) {
        if (MDNode *N = MD.second) {
          if (auto *subProgram = dyn_cast<DISubprogram>(N)) {
            funcfile = subProgram->getFilename().str();
            funcdir = subProgram->getDirectory().str();

            if (!funcfile.empty() && !funcdir.empty()){
              // fix path here; if "funcfile" does not start with "/", use funcdir as the prefix of funcfile
              if (!startsWith(funcfile, "/")){
                func_abs_path = funcdir;
                func_abs_path.append("/");
                func_abs_path.append(funcfile);
              } else func_abs_path = funcfile;

              // get the real path for the current file
              char *realp = realpath(func_abs_path.c_str(), NULL);
              if (realp == NULL) git_no_found = 1;
              else{
                func_abs_path.assign(realp);
                free(realp);
                git_no_found = 0;
              }

              if (!git_no_found){
                /* Directory of the file. */
                func_abs_path = func_abs_path.substr(0, func_abs_path.find_last_of("\\/")); //remove filename in string
                //git rev-parse --show-toplevel: show the root folder of a repository
                // result: /home/usr/repo_name
                std::string cmd_repo ("git rev-parse --show-toplevel 2>&1");
                
                git_path = execute_git_cmd(func_abs_path, cmd_repo);
                if (git_path.empty()) git_no_found = 1;
                else git_path.append("/"); // result: /home/usr/repo_name/
                
                /* Check shallow git repository */
                // git rev-list HEAD --count: count the number of commits
                if (!git_no_found){
                  std::string cmd_count ("git rev-list HEAD --count 2>&1");
                  std::string commit_cnt = execute_git_cmd(git_path, cmd_count);
                  
                  if (commit_cnt.compare("1") == 0){ //only one commit
                    git_no_found = 1;
                    is_one_commit = 1;
                    OKF("Shallow repository clone. Ignoring file %s.", funcfile.c_str());
                    break;
                  }
                  // #change threshold
                  // changes_inst_threshold = get_threshold_changes(git_path);
                  //get commit time
                  std::string head_cmd("git show -s --format=%ct HEAD");
                  head_commit_days = get_commit_time_days(git_path, head_cmd);
                  std::string init_cmd("git log --reverse --date=unix --oneline --format=%cd | head -n1");
                  init_commit_days = get_commit_time_days(git_path, init_cmd);
                  /* Get the number of commits before HEAD */
                  head_num_parents = get_max_ranks(git_path);
                  /* thresholds */
                  norm_change_thd = inst_norm_change(THRESHOLD_CHANGES, change_sig);
                  norm_age_thd = inst_norm_age(head_commit_days - init_commit_days, THRESHOLD_DAYS);
                  norm_rank_thd = inst_norm_rank(head_num_parents, THRESHOLD_RANKS);
                  break;
                }
                
              }
          
            }
              
          }
        }
      }
    }
    
    for (auto &BB : F) {
      
      BasicBlock::iterator IP = BB.getFirstInsertionPt();
      IRBuilder<> IRB(&(*IP));

      if (AFL_R(100) >= inst_ratio) continue;

      /* Make up cur_loc */

      unsigned int cur_loc = AFL_R(MAP_SIZE);

      ConstantInt *CurLoc = ConstantInt::get(Int32Ty, cur_loc);

      double bb_rank_age = 0, bb_age_best = 0, bb_burst_best = 0, bb_rank_best = 0;
      double bb_raw_fitness, tmp_score;
      bool bb_raw_fitness_flag = false;
      
      if (!bb_lines.empty())
            bb_lines.clear();
      bb_lines.insert(0);
      
      for (auto &I: BB){
  
        line = 0;
        std::string filename, filedir, clean_relative_path;
        /* Connect targets with instructions */
        DILocation *Loc = I.getDebugLoc().get(); 
        if (Loc && !git_no_found){
          filename = Loc->getFilename().str();
          filedir = Loc->getDirectory().str();
          line = Loc->getLine();
          if (filename.empty()){
            DILocation *oDILoc = Loc->getInlinedAt();
            if (oDILoc){
              line = oDILoc->getLine();
              filename = oDILoc->getFilename().str();
              filedir = oDILoc->getDirectory().str();
            }
          }

          /* take care of git blame path: relative to repo dir */
          if (!filename.empty() && !filedir.empty()){
            
            clean_relative_path = get_file_path_relative_to_git_dir(filename, filedir, git_path);
            
            if (!clean_relative_path.empty()){
              /* calculate score of a block */
                /* Check if file exists in HEAD using command mode */
              if (unexist_files.count(clean_relative_path)) break;

              if (!bb_lines.count(line)){
                bb_lines.insert(line);
                /* process files that have not been processed */
                if (!processed_files.count(clean_relative_path)){
                  processed_files.insert(clean_relative_path);

                  /* Check if file exists in HEAD using command mode */
                  if (!is_file_exist(clean_relative_path, git_path)){
                    unexist_files.insert(clean_relative_path);
                    break;
                  }
                  
                  /* the ages for lines */
                  if (use_cmd_age) 
                    calculate_line_age_git_cmd(clean_relative_path, git_path, map_age_scores,
                                                head_commit_days, init_commit_days);
                  if (use_cmd_age_rank)
                    cal_line_age_rank(clean_relative_path, git_path, map_rank_age, 
                                            commit_rank, head_num_parents);
                  /* the number of changes for lines */
                  if (use_cmd_change)
                    calculate_line_change_git_cmd(clean_relative_path, git_path, 
                                                      map_bursts_scores, change_sig);
                  
                }
                
                if (use_cmd_age){
                  // calculate line age
                  if (map_age_scores.count(clean_relative_path)){
                    if (map_age_scores[clean_relative_path].count(line)){
                      // use the best value of a line as the value of a BB
                      tmp_score = map_age_scores[clean_relative_path][line];
                      if (bb_age_best < tmp_score) bb_rank_age = bb_age_best = tmp_score;
                    }
                  }
                }

                if (use_cmd_age_rank){
                  if (map_rank_age.count(clean_relative_path)){
                    if (map_rank_age[clean_relative_path].count(line)){
                      tmp_score = map_rank_age[clean_relative_path][line];
                      if (bb_rank_best < tmp_score) bb_rank_age = bb_rank_best = tmp_score;
                    }
                  }
                }

                if (use_cmd_change){
                  // calculate line change
                  if (map_bursts_scores.count(clean_relative_path)){
                    if (map_bursts_scores[clean_relative_path].count(line)){
                      tmp_score = map_bursts_scores[clean_relative_path][line];
                      if (bb_burst_best < tmp_score) bb_burst_best = tmp_score;
                    }
                  }
                }
              
              }
            }
          

          }
        }
      } 
 
      /* Load prev_loc */

      LoadInst *PrevLoc = IRB.CreateLoad(AFLPrevLoc);
      PrevLoc->setMetadata(NoSanMetaId, NoneMetaNode);
      Value *PrevLocCasted = IRB.CreateZExt(PrevLoc, IRB.getInt32Ty());

      /* Load SHM pointer */
      
      LoadInst *MapPtr = IRB.CreateLoad(AFLMapPtr);
      MapPtr->setMetadata(NoSanMetaId, NoneMetaNode);
      Value *MapPtrIdx =
          IRB.CreateGEP(MapPtr, IRB.CreateXor(PrevLocCasted, CurLoc));

      /* Update bitmap */

      LoadInst *Counter = IRB.CreateLoad(MapPtrIdx);
      Counter->setMetadata(NoSanMetaId, NoneMetaNode);
      Value *Incr = IRB.CreateAdd(Counter, ConstantInt::get(Int8Ty, 1));
      IRB.CreateStore(Incr, MapPtrIdx)
          ->setMetadata(NoSanMetaId, NoneMetaNode);


      /* Set prev_loc to cur_loc >> 1 */

      StoreInst *Store =
          IRB.CreateStore(ConstantInt::get(Int32Ty, cur_loc >> 1), AFLPrevLoc);
      Store->setMetadata(NoSanMetaId, NoneMetaNode);

      /* insert age/churn into BBs */
      if ((use_cmd_age || use_cmd_age_rank) && !use_cmd_change){
        /* Age only; Add age of lines */
        if ((bb_rank_age > 0) && //only when age is assigned
                  (bb_age_best > norm_age_thd || bb_rank_best > norm_rank_thd
                      || AFL_R(100) < bb_select_ratio)){

          inst_ages ++;
          module_total_ages += bb_rank_age;

          bb_raw_fitness = bb_rank_age;
          bb_raw_fitness_flag = true;

          inst_fitness ++;
          module_total_fitness += bb_raw_fitness;
          
        }
      } else if (use_cmd_change && !use_cmd_age_rank && !use_cmd_age){
        /* Change Only; Add changes of lines */
        if ((bb_burst_best > 0) && //only when change is assigned
                (bb_burst_best > norm_change_thd || AFL_R(100) < bb_select_ratio)){
          inst_changes++;
          module_total_changes += bb_burst_best;

          bb_raw_fitness = bb_burst_best;
          bb_raw_fitness_flag = true;

          inst_fitness ++;
          module_total_fitness += bb_raw_fitness;
        }
      } else if ((use_cmd_age || use_cmd_age_rank) && use_cmd_change){
        /* both age and change are enabled */
        if ((bb_rank_age > 0 || bb_burst_best > 0) &&
                (bb_burst_best > norm_change_thd || bb_age_best > norm_age_thd
                   || bb_rank_best > norm_rank_thd || AFL_R(100) < bb_select_ratio)){
            // change
            if (bb_burst_best > 0){
              inst_changes++;
              module_total_changes += bb_burst_best;
            } else bb_burst_best = 1;
            // age
            if (bb_rank_age > 0){
              inst_ages ++;
              module_total_ages += bb_rank_age;
            } else bb_rank_age = 1;
            // combine
            bb_raw_fitness = bb_burst_best * bb_rank_age;
            bb_raw_fitness_flag = true;

            inst_fitness ++;
            module_total_fitness += bb_raw_fitness;
          
        }
        
      }

      if (bb_raw_fitness_flag) {
        Constant *Weight = ConstantFP::get(DoubleTy, bb_raw_fitness);
        Constant *MapLoc = ConstantInt::get(Int32Ty, MAP_SIZE);
        Constant *MapCntLoc = ConstantInt::get(Int32Ty, MAP_SIZE + 8);
        
        // add to shm, churn raw fitness
        Value *MapWtPtr = IRB.CreateGEP(MapPtr, MapLoc);
        LoadInst *MapWt = IRB.CreateLoad(DoubleTy, MapWtPtr);
        MapWt->setMetadata(NoSanMetaId, NoneMetaNode);
        Value *IncWt = IRB.CreateFAdd(MapWt, Weight);
        IRB.CreateStore(IncWt, MapWtPtr)
          ->setMetadata(NoSanMetaId, NoneMetaNode);

        // add to shm, block count
#ifdef WORD_SIZE_64
        Value *MapCntPtr = IRB.CreateGEP(MapPtr, MapCntLoc);
        LoadInst *MapCnt = IRB.CreateLoad(Int64Ty, MapCntPtr);
        MapCnt->setMetadata(NoSanMetaId, NoneMetaNode);
        Value *IncCnt = IRB.CreateAdd(MapCnt, ConstantInt::get(Int64Ty, 1));
        IRB.CreateStore(IncCnt, MapCntPtr)
                ->setMetadata(NoSanMetaId, NoneMetaNode);
#else
        Value *MapCntPtr = IRB.CreateGEP(MapPtr, MapCntLoc);
        LoadInst *MapCnt = IRB.CreateLoad(Int32Ty, MapCntPtr);
        MapCnt->setMetadata(NoSanMetaId, NoneMetaNode);
        Value *IncCnt = IRB.CreateAdd(MapCnt, ConstantInt::get(Int32Ty, 1));
        IRB.CreateStore(IncCnt, MapCntPtr)
                ->setMetadata(NoSanMetaId, NoneMetaNode);

#endif
      }

      inst_blocks++;

    }
  }

  /* Say something nice. */

  if (!be_quiet) {

    if (!inst_blocks) WARNF("No instrumentation targets found.");
    else OKF("Instrumented %u locations (%s mode, ratio %u%%).",
             inst_blocks, getenv("AFL_HARDEN") ? "hardened" :
             ((getenv("AFL_USE_ASAN") || getenv("AFL_USE_MSAN")) ?
              "ASAN/MSAN" : "non-hardened"), inst_ratio);
    OKF("AFLChurn instrumentation ratio %u%%", bb_select_ratio);
    if (inst_ages) module_ave_ages = module_total_ages / inst_ages;
    if (inst_changes) module_ave_chanegs = module_total_changes / inst_changes;
    if (inst_fitness) module_ave_fitness = module_total_fitness / inst_fitness;

    if (use_cmd_age && !is_one_commit){
      OKF("Using Age. Counted %u BBs with the average of f(days)=%.6f ages.", 
              inst_ages, module_ave_ages);
    } else if (use_cmd_age_rank && !is_one_commit){
      OKF("Using Rank. Counted %u BBs with the average age of f(rank)=%.6f commits.", 
                  inst_ages, module_ave_ages);
    }
    if (use_cmd_change && !is_one_commit){
      OKF("Using Change. Counted %u BBs with the average churn of f(changes)=%.6f churns.",
                    inst_changes, module_ave_chanegs);
    } 

    OKF("BB Churn Raw Fitness. Instrumented %u BBs with average raw fitness of %.6f",
                    inst_fitness, module_ave_fitness);
      

  }

  return true;

}


static void registerAFLPass(const PassManagerBuilder &,
                            legacy::PassManagerBase &PM) {

  PM.add(new AFLCoverage());

}

// TODO: which one? early or last? - rosen

// static RegisterStandardPasses RegisterAFLPass(
//     PassManagerBuilder::EP_ModuleOptimizerEarly, registerAFLPass);

static RegisterStandardPasses RegisterAFLPass(
    PassManagerBuilder::EP_OptimizerLast, registerAFLPass);


static RegisterStandardPasses RegisterAFLPass0(
    PassManagerBuilder::EP_EnabledOnOptLevel0, registerAFLPass);
