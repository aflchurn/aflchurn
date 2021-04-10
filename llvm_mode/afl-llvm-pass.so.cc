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



/* churn signal */
enum{
  /* 00 */ SIG_XLOG_CHANGES,
  /* 01 */ SIG_LOG_CHANGES
};


using namespace llvm;


namespace {

  class AFLCoverage : public ModulePass {

    public:

      static char ID;
      
      // Type *VoidTy;
      // IntegerType *Int1Ty;
      // IntegerType *Int8Ty;
      // IntegerType *Int32Ty;
      // IntegerType *Int64Ty;
      // Type *Int8PtrTy;
      // Type *Int64PtrTy;
      // GlobalVariable *AFLMapPtr;
      // GlobalVariable *AFLPrevLoc;

      // unsigned NoSanMetaId;
      // MDTuple *NoneMetaNode;

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
    str_res.assign(ch_git_res);  //, strlen(ch_git_res)
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
                    std::map<std::string, std::map<unsigned int, unsigned int>> &file2line2change_map,
                    u8 change_type){
    
  std::ostringstream cmd;
  std::string str_cur_commit_sha;
  char ch_cur_commit_sha[128];
  int rc = 0;
  FILE *fp;
  std::set<unsigned int> changed_lines_cur_commit;
  std::map <unsigned int, unsigned int> lines2changes, tmp_line2changes;
  
  // get the commits that change the file of relative_file_path
  // result: commit short SHAs
  // TODO: If the file name changed, it cannot get the changed lines.
  //  --since=10.years 
  char* ch_month = getenv("BURST_SINCE_MONTHS");
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
    switch(change_type){
      case SIG_XLOG_CHANGES:
        for (auto l2c : lines2changes){
          if (l2c.second < 0) tmp_line2changes[l2c.first] = 0;
          else tmp_line2changes[l2c.first] = (l2c.second + 1) * log2(l2c.second + 1); 
        }
        file2line2change_map[relative_file_path] = tmp_line2changes;
        break;

      case SIG_LOG_CHANGES:
        for (auto l2c : lines2changes){
          if (l2c.second < 0) tmp_line2changes[l2c.first] = 0;
          else tmp_line2changes[l2c.first] = log2(l2c.second + 1) * 100;
        }
        file2line2change_map[relative_file_path] = tmp_line2changes;
        break;
        
    }
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
                    std::map<std::string, std::map<unsigned int, unsigned int>> &file2line2age_map){

  std::map<unsigned int, unsigned int> line_age_days;

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

  
  /* Get date (unix time) of HEAD commit */
  unsigned long head_time;
  FILE *dfp;
  std::ostringstream datecmd;
  datecmd << "cd " << git_directory
          << " && git show -s --format=%ct HEAD";
  dfp = popen(datecmd.str().c_str(), "r");
  if (NULL == dfp) return false;
  if (fscanf(dfp, "%lu", &head_time) != 1) return false;
  pclose(dfp);


  cmd << "cd " << git_directory << " && git blame --date=unix " << relative_file_path
        << " | grep -o -P \"[0-9]{9}[0-9]? +[0-9]+\"";

  fp = popen(cmd.str().c_str(), "r");
  if(NULL == fp) return false;
  // get line by line
  while(fscanf(fp, "%lu %u", &unix_time, &line) == 2){
    days_since_last_change = (head_time - unix_time) / 86400; //days

    // if (days_since_last_change < 0) line_age_days[line] = 10000;
    // else line_age_days[line] = 
    //     10000 / (log2(days_since_last_change + 2) * log2(days_since_last_change + 2));
    if (days_since_last_change <= 0) line_age_days[line] = 10000;
    else line_age_days[line] = 10000 / (double)days_since_last_change;
    
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
                std::map<std::string, std::map<unsigned int, unsigned int>> &file2line2rank_map,
                std::map<std::string, unsigned int> &commit2rank){

  char line_commit_sha[256];
  FILE *dfp, *curdfp;
  unsigned int nothing_line, line_num;
  std::map<unsigned int, unsigned int> line_rank;

  /* Get the number of commits before HEAD */
  unsigned int head_num_parents, cur_num_parents;
  int rank4line;
  std::ostringstream headcmd;
  headcmd << "cd " << git_directory
          << " && git rev-list --count HEAD";
  dfp = popen(headcmd.str().c_str(), "r");
  if (NULL == dfp) return false;
  if (fscanf(dfp, "%u", &head_num_parents) != 1) return false;
  pclose(dfp);

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
        // rlogrank
        // if (rank4line < 0) commit2rank[str_cmt] = line_rank[line_num] = 1000;
        // else commit2rank[str_cmt] = line_rank[line_num] = 1000 / log2(rank4line + 2);

        // log2rank ==> CAUTION: should minimize fitness in afl-fuzz.c
        // if (rank4line < 0) commit2rank[str_cmt] = line_rank[line_num] = 0;
        // else commit2rank[str_cmt] = line_rank[line_num] = log2(rank4line + 1) * 100;

        /* rrank */
        if (rank4line <= 0) commit2rank[str_cmt] = line_rank[line_num] = 100000;
        else commit2rank[str_cmt] = line_rank[line_num] = 100000 / rank4line;

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

  bool use_cmd_change = false, use_cmd_age_rank = false, use_cmd_age = false;

  
  u8 change_signal_type = 1; // default: logchange

  char *churn_sig, *day_sig;

  if (getenv("BURST_COMMAND_AGE")) use_cmd_age = true;
  day_sig = getenv("BURST_AGE_SIGNAL");
  if (day_sig){
    if (!use_cmd_age) FATAL("Please export BURST_COMMAND_AGE=1 first ");
    if (!strcmp(day_sig, "rrank")){
      use_cmd_age_rank = true;
      use_cmd_age = false;
    } else{
      FATAL("Set proper age signal");
    }
  }

  if (getenv("BURST_COMMAND_CHURN")) use_cmd_change = true;
  churn_sig = getenv("BURST_CHURN_SIGNAL");
  if (churn_sig){
    if (!use_cmd_change) FATAL("Please export BURST_COMMAND_CHURN first ");
    if (!strcmp(churn_sig, "xlogchange")){
      change_signal_type = SIG_XLOG_CHANGES;
    } else if (!strcmp(churn_sig, "logchange")){
      change_signal_type = SIG_LOG_CHANGES;
    } else{
      FATAL("Set proper churn signal");
    }
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

  int inst_blocks = 0, inst_ages = 0, inst_changes = 0,
      module_total_ages = 0, module_total_changes = 0,
      module_ave_ages = 0, module_ave_chanegs = 0;

  std::set<unsigned int> bb_lines;
  std::set<std::string> unexist_files, processed_files;
  unsigned int line;
  std::string git_path;
  
  int git_no_found = 1, // 0: found; otherwise, not found
      is_one_commit = 0; // don't calculate for --depth 1

  std::map<std::string, unsigned int> commit_rank;
  // file name (relative path): line NO. , score
  std::map<std::string, std::map<unsigned int, unsigned int>> map_age_scores, map_bursts_scores, map_rank_age;

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
   
                  if (!git_no_found) break;
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

      unsigned int bb_age_total = 0, bb_age_avg = 0, bb_age_count = 0;
      unsigned int bb_burst_total = 0, bb_burst_avg = 0, bb_burst_count = 0;
      unsigned int bb_rank_total = 0, bb_rank_avg = 0, bb_rank_count = 0;
      
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
            // std::cout << "file name: " << filename << std::endl << "file dir: " << filedir <<std::endl;
            clean_relative_path = get_file_path_relative_to_git_dir(filename, filedir, git_path);
            // std::cout << "relative path: " << clean_relative_path << std::endl;
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
                    calculate_line_age_git_cmd(clean_relative_path, git_path, map_age_scores);
                  if (use_cmd_age_rank)
                    cal_line_age_rank(clean_relative_path, git_path, map_rank_age, commit_rank);
                  /* the number of changes for lines */
                  if (use_cmd_change)
                    calculate_line_change_git_cmd(clean_relative_path, git_path, map_bursts_scores, change_signal_type);
                  
                }
                
                if (use_cmd_age){
                  // calculate line age
                  if (map_age_scores.count(clean_relative_path)){
                    if (map_age_scores[clean_relative_path].count(line)){
                      bb_age_total += map_age_scores[clean_relative_path][line];
                      bb_age_count++;
                    }
                  }
                }

                if (use_cmd_age_rank){
                  if (map_rank_age.count(clean_relative_path)){
                    if (map_rank_age[clean_relative_path].count(line)){
                      bb_rank_total += map_rank_age[clean_relative_path][line];
                      bb_rank_count++;
                    }
                  }
                }

                if (use_cmd_change){
                  // calculate line change
                  if (map_bursts_scores.count(clean_relative_path)){
                    if (map_bursts_scores[clean_relative_path].count(line)){
                      bb_burst_total += map_bursts_scores[clean_relative_path][line];
                      bb_burst_count ++;
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

      /* Add age of lines */
      if (bb_age_count > 0 || bb_rank_count > 0){ //only when age is assigned
        if (bb_age_count > 0)
          bb_age_avg = bb_age_total / bb_age_count;
        else if (bb_rank_count > 0){
          bb_rank_avg = bb_rank_total / bb_rank_count;
          bb_age_avg = bb_rank_avg;
        }
        
        inst_ages ++;
        module_total_ages += bb_age_avg;
        //std::cout << "block id: "<< cur_loc << ", bb age: " << (float)bb_age_avg << std::endl;
#ifdef WORD_SIZE_64
        Type *AgeLargestType = Int64Ty;
        Constant *MapAgeLoc = ConstantInt::get(AgeLargestType, MAP_SIZE);
        Constant *MapAgeCntLoc = ConstantInt::get(AgeLargestType, MAP_SIZE + 8);
        Constant *AgeWeight = ConstantInt::get(AgeLargestType, bb_age_avg);
#else
        Type *AgeLargestType = Int32Ty;
        Constant *MapAgeLoc = ConstantInt::get(AgeLargestType, MAP_SIZE);
        Constant *MapAgeCntLoc = ConstantInt::get(AgeLargestType, MAP_SIZE + 4);
        Constant *AgeWeight = ConstantInt::get(AgeLargestType, bb_age_avg);
#endif
        // add to shm, age
        Value *MapAgeWtPtr = IRB.CreateGEP(MapPtr, MapAgeLoc);
        LoadInst *MapAgeWt = IRB.CreateLoad(AgeLargestType, MapAgeWtPtr);
        MapAgeWt->setMetadata(NoSanMetaId, NoneMetaNode);
        Value *IncAgeWt = IRB.CreateAdd(MapAgeWt, AgeWeight);
        
        IRB.CreateStore(IncAgeWt, MapAgeWtPtr)
          ->setMetadata(NoSanMetaId, NoneMetaNode);
        // add to shm, block count
        Value *MapAgeCntPtr = IRB.CreateGEP(MapPtr, MapAgeCntLoc);
        LoadInst *MapAgeCnt = IRB.CreateLoad(AgeLargestType, MapAgeCntPtr);
        MapAgeCnt->setMetadata(NoSanMetaId, NoneMetaNode);
        Value *IncAgeCnt = IRB.CreateAdd(MapAgeCnt, ConstantInt::get(AgeLargestType, 1));
        IRB.CreateStore(IncAgeCnt, MapAgeCntPtr)
                ->setMetadata(NoSanMetaId, NoneMetaNode);
      }

      /* Add changes of lines */
      if (bb_burst_count > 0){ //only when change is assigned
        bb_burst_avg = bb_burst_total / bb_burst_count;
        inst_changes++;
        module_total_changes += bb_burst_avg;
        // std::cout << "block id: "<< cur_loc << ", bb change: " << bb_burst_avg << std::endl;
#ifdef WORD_SIZE_64
        Type *ChangeLargestType = Int64Ty;
        Constant *MapChangeLoc = ConstantInt::get(ChangeLargestType, MAP_SIZE + 16);
        Constant *MapChangeCntLoc = ConstantInt::get(ChangeLargestType, MAP_SIZE + 24);
        Constant *ChangeWeight = ConstantInt::get(ChangeLargestType, bb_burst_avg);
#else
        Type *ChangeLargestType = Int32Ty;
        Constant *MapChangeLoc = ConstantInt::get(ChangeLargestType, MAP_SIZE + 8);
        Constant *MapChangeCntLoc = ConstantInt::get(ChangeLargestType, MAP_SIZE + 12);
        Constant *ChangeWeight = ConstantInt::get(ChangeLargestType, bb_burst_avg);
#endif
        // add to shm, changes
        Value *MapChangeWtPtr = IRB.CreateGEP(MapPtr, MapChangeLoc);
        LoadInst *MapChangeWt = IRB.CreateLoad(ChangeLargestType, MapChangeWtPtr);
        MapChangeWt->setMetadata(NoSanMetaId, NoneMetaNode);
        Value *IncChangeWt = IRB.CreateAdd(MapChangeWt, ChangeWeight);
        
        IRB.CreateStore(IncChangeWt, MapChangeWtPtr)
          ->setMetadata(NoSanMetaId, NoneMetaNode);
        // add to shm, block count
        Value *MapChangeCntPtr = IRB.CreateGEP(MapPtr, MapChangeCntLoc);
        LoadInst *MapChangeCnt = IRB.CreateLoad(ChangeLargestType, MapChangeCntPtr);
        MapChangeCnt->setMetadata(NoSanMetaId, NoneMetaNode);
        Value *IncChangeCnt = IRB.CreateAdd(MapChangeCnt, ConstantInt::get(ChangeLargestType, 1));
        IRB.CreateStore(IncChangeCnt, MapChangeCntPtr)
                ->setMetadata(NoSanMetaId, NoneMetaNode);
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
    
    if (inst_ages) module_ave_ages = module_total_ages / inst_ages;
    if (inst_changes) module_ave_chanegs = module_total_changes / inst_changes;
    if (use_cmd_age && !is_one_commit){
      OKF("Using command line git. Instrumented %u BBs with the average of f(days)=%d ages.", 
              inst_ages, module_ave_ages);
    } else if (use_cmd_age_rank && !is_one_commit){
      OKF("Using command line git. Instrumented %u BBs with the average of f(rank)=%d commits.", 
                  inst_ages, module_ave_ages);
    }
    if (use_cmd_change && !is_one_commit){
      switch (change_signal_type){
        case SIG_XLOG_CHANGES:
          OKF("Using command line git. Instrumented %u BBs with the average churn of xlogx=%u churns.",
                    inst_changes, module_ave_chanegs);
          break;

        case SIG_LOG_CHANGES:
          OKF("Using command line git. Instrumented %u BBs with the average churn of log2(changes)*100=%d churns.",
                    inst_changes, module_ave_chanegs);
          break;
      }
        
    } 
      

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
