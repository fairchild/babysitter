/*---------------------------- Includes ------------------------------------*/
#include <string>
#include <unistd.h>
#include <fcntl.h>
#include <sstream>
#include <iomanip>
#include <assert.h>
#include <fcntl.h>
#include <gelf.h>
#include <regex.h>
#include <libgen.h>
// System
#include <sys/types.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/resource.h>
// Include sys/capability if the system can handle it. 
#ifdef HAVE_SYS_CAPABILITY_H
#include <sys/capability.h>
#endif
// Erlang interface
#include "ei++.h"

#include "honeycomb.h"
#include "worker_bee.h"

/*---------------------------- Implementation ------------------------------*/

using namespace ei;

int Honeycomb::setup_defaults() {
  /* Setup environment defaults */
  const char* default_env_vars[] = {
   "LD_LIBRARY_PATH=/lib;/usr/lib;/usr/local/lib", 
   "HOME=/mnt"
  };
  
  int m_cenv_c = 0;
  const int max_env_vars = 1000;
  
  if ((m_cenv = (const char**) new char* [max_env_vars]) == NULL) {
    m_err << "Could not allocate enough memory to create list"; return -1;
  }
  
  memcpy(m_cenv, default_env_vars, (std::min((int)max_env_vars, (int)sizeof(default_env_vars)) * sizeof(char *)));
  m_cenv_c = sizeof(default_env_vars) / sizeof(char *);
  
  return 0;
}

/**
 * Decode the erlang tuple
 * The erlang tuple decoding happens for all the tuples that get sent over the wire
 * to the c-node.
 **/
int Honeycomb::ei_decode(ei::Serializer& ei) {
  // {Cmd::string(), [Option]}
  //      Option = {env, Strings} | {cd, Dir} | {kill, Cmd}
  int sz;
  std::string op_str, val;
  
  m_err.str("");
  delete [] m_cenv;
  m_cenv = NULL;
  m_env.clear();
  m_nice = INT_MAX;
    
  if (ei.decodeString(m_cmd) < 0) {
    m_err << "badarg: cmd string expected or string size too large" << m_cmd << "Command!";
    return -1;
  } else if ((sz = ei.decodeListSize()) < 0) {
    m_err << "option list expected";
    return -1;
  } else if (sz == 0) {
    m_cd  = "";
    m_kill_cmd = "";
    return 0;
  }
  
  // Run through the commands and decode them
  enum OptionT            { CD,   ENV,   KILL,   NICE,   USER,   STDOUT,   STDERR,  MOUNT } opt;
  const char* options[] = {"cd", "env", "kill", "nice", "user", "stdout", "stderr", "mount"};
  
  for(int i=0; i < sz; i++) {
    if (ei.decodeTupleSize() != 2 || (int)(opt = (OptionT)ei.decodeAtomIndex(options, op_str)) < 0) {
      m_err << "badarg: cmd option must be an atom"; 
      return -1;
    }
    DEBUG_MSG("Found option: %s\n", op_str.c_str());
    switch(opt) {
      case CD:
      case KILL:
      case USER: {
        // {cd, Dir::string()} | {kill, Cmd::string()} | {user, Cmd::string()} | etc.
        if (ei.decodeString(val) < 0) {m_err << opt << " bad option"; return -1;}
        if (opt == CD) {m_cd = val;}
        else if (opt == KILL) {m_kill_cmd = val;}
        else if (opt == USER) {
          struct passwd *pw = getpwnam(val.c_str());
          if (pw == NULL) {m_err << "Invalid user: " << val << " : " << ::strerror(errno); return -1;}
          m_user = pw->pw_uid;
        }
        break;
      }
      case NICE: {
        if (ei.decodeInt(m_nice) < 0 || m_nice < -20 || m_nice > 20) {m_err << "Nice must be between -20 and 20"; return -1;}
        break;
      }
      case ENV: {
        int env_sz = ei.decodeListSize();
        if (env_sz < 0) {m_err << "Must pass a list for env option"; return -1;}
        
        for(int i=0; i < env_sz; i++) {
          std::string str;
          if (ei.decodeString(str) >= 0) {
            m_env.push_back(str);
            m_cenv[m_cenv_c+i] = m_env.back().c_str();
          } else {m_err << "Invalid env argument at " << i; return -1;}
        }
        m_cenv[m_cenv_c+1] = NULL; // Make sure we have a NULL terminated list
        m_cenv_c = env_sz+m_cenv_c; // save the new value... we don't really need to do this, though
        break;
      }
      case STDOUT:
      case STDERR: {
        int t = 0;
        int sz = 0;
        std::string str, fop;
        t = ei.decodeType(sz);
        if (t == ERL_ATOM_EXT) ei.decodeAtom(str);
        else if (t == ERL_STRING_EXT) ei.decodeString(str);
        else {
          m_err << "Atom or string tuple required for " << op_str;
          return -1;
        }
        // Setup the writer
        std::string& rs = (opt == STDOUT) ? m_stdout : m_stderr;
        std::stringstream stream;
        int fd = (opt == STDOUT) ? 1 : 2;
        if (str == "null") {stream << fd << ">/dev/null"; rs = stream.str();}
        else if (str == "stderr" && opt == STDOUT) {rs = "1>&2";}
        else if (str == "stdout" && opt == STDERR) {rs = "2>&1";}
        else if (str != "") {stream << fd << ">\"" << str << "\"";rs = stream.str();}
        break;
      }
      default:
        m_err << "bad options: " << op_str; return -1;
    }
  }
  
  if (m_stdout == "1>&2" && m_stderr != "2>&1") {
    m_err << "cirtular reference of stdout and stderr";
    return -1;
  } else if (!m_stdout.empty() || !m_stderr.empty()) {
    std::stringstream stream; stream << m_cmd;
    if (!m_stdout.empty()) stream << " " << m_stdout;
    if (!m_stderr.empty()) stream << " " << m_stderr;
    m_cmd = stream.str();
  }
  
  return 0;
}

int Honeycomb::build_and_execute(std::string confinement_root, mode_t confinement_mode) {
  if (build_environment(confinement_root, confinement_mode)) {
   fprintf(stderr, "Failed to build_environment in '%s'\n", confinement_root.c_str());
   return -1;
  }
  if (execute()) {
   fprintf(stderr, "Failed to execute in '%s'\n", confinement_root.c_str());
   return -1;
  }
  return 0;
}

int Honeycomb::build_environment(std::string confinement_root, mode_t confinement_mode) {
  /* Prepare as of the environment from the child process */
  setup_defaults();
  // First, get a random_uid to run as
  m_user = random_uid();
  m_group = m_user;
  gid_t egid = getegid();
  
  // Create the root directory, the 'cd' if it's not specified
  if (m_cd.empty()) {
    struct stat stt;
    if (0 == stat(confinement_root.c_str(), &stt)) {
      if (0 != stt.st_uid || 0 != stt.st_gid) {
        m_err << confinement_root << " is not owned by root (0:0). Exiting..."; return -1;
      }
    } else if (ENOENT == errno) {
      setegid(0);
    
      if(mkdir(confinement_root.c_str(), confinement_mode)) {
        m_err << "Could not create the directory " << confinement_root; return -1;
      }
    } else {
      m_err << "Unknown error: " << ::strerror(errno); return -1;
    }
    // The m_cd path is the confinement_root plus the user's uid
    m_cd = confinement_root + "/" + to_string(m_user, 10);
  
    if (mkdir(m_cd.c_str(), confinement_mode)) {
      m_err << "Could not create the new confinement root " << m_cd;
    }
    
    if (chown(m_cd.c_str(), m_user, m_user)) {
      m_err << "Could not chown to the effective user: " << m_user; return -1;
    } 
    setegid(egid); // Return to be the effective user
  }
  
  DEBUG_MSG("Created root directory at: '%s'\n", m_cd.c_str());

  DEBUG_MSG("Don't choot says: %i\n", m_dont_chroot);

  // Next, create the chroot, if necessary which sets up a chroot
  // environment in the confinement_root path
  if (!m_dont_chroot) {
    pid_t child = fork();
    
DEBUG_MSG("forked... %i\n", child);

    if(0 == child) {
      // Find the binary command
      // std::string binary_path = find_binary(m_cmd);
      // std::string pth = m_cd + binary_path;
      
      temp_drop();

      // Currently, we only support running binaries, not shell scripts
      printf("----- Build the chroot please\n");
      copy_deps(m_cd, m_cmd);
      
      //cp_r(binary_path, pth);

      // if (chmod(pth.c_str(), S_IREAD|S_IEXEC|S_IWRITE)) {
      //   fprintf(stderr, "Could not change permissions to '%s' make it executable\n", pth.c_str());
      //   return -1;
      // }
      
      // come back to our permissions
      restore_perms();
      
      // Chroot
      // Secure the chroot first
      unsigned int fd, fd_max;
      struct stat buf; struct rlimit lim;
      if (! getrlimit(RLIMIT_NOFILE, &lim) && (fd_max < lim.rlim_max)) fd_max = lim.rlim_max;
      
      // compute the number of file descriptors that can be open
#ifdef OPEN_MAX
  fd_max = OPEN_MAX;
#elif defined(NOFILE)
  fd_max = NOFILE;
#else
  fd_max = getdtablesize();
#endif

      // close all file descriptors except stdin,stdout,stderr
      // because they are security issues
      DEBUG_MSG("Securing the chroot environment at '%s'\n", m_cd.c_str());
      for (fd=2;fd < fd_max; fd++) {
        if ( !fstat(fd, &buf) && S_ISDIR(buf.st_mode))
          if (close(fd)) {
            fprintf(stderr, "Could not close insecure directory/file: %i before chrooting\n", fd);
            return(-1);
          }
      }
      
      // Change directory
      if (chdir(m_cd.c_str())) {
        fprintf(stderr, "Could not chdir into %s before chrooting\n", m_cd.c_str());
        return(-1);
      }

      // Chroot please
      if (chroot(m_cd.c_str())) {
        fprintf(stderr, "Could not chroot into %s\n", m_cd.c_str());
        return(-1);
      }
      
      // Set resource limitations
      // TODO: Make extensible
      temp_drop();
      set_rlimits();
      restore_perms();
      exit(0);
      return(0); // Exit from the child pid
    }
  } else {
    // Cd into the working directory directory
    if (chdir(m_cd.c_str())) {
      fprintf(stderr, "Could not chdir into %s with no chrooting\n", m_cd.c_str());
      return(-1);
    }  }
  
  // Success!
  return 0;
}

pid_t Honeycomb::execute() {
  pid_t chld = fork();
  
  // We are in the child pid
  perm_drop(); // Drop into new user forever!!!!
  
  if(chld < 0) {
    fprintf(stderr, "Could not fork into new process :(\n");
    return(-1);
   } else { 
    // Build the environment vars
    const std::string shell = getenv("SHELL");
    const std::string shell_args = "-c";
    const char* argv[] = { shell.c_str(), shell_args.c_str(), m_cmd.c_str() };
   
    if (execve(m_cmd.c_str(), (char* const*)argv, (char* const*)m_cenv) < 0) {
      fprintf(stderr, "Cannot execute '%s' because '%s'", m_cmd.c_str(), ::strerror(errno));
      return EXIT_FAILURE;
    }
  } 
  
  if (m_nice != INT_MAX && setpriority(PRIO_PROCESS, chld, m_nice) < 0) {
    fprintf(stderr, "Cannot set priority of pid %d", chld);
    return(-1);
  }
  
  return chld;
}

void Honeycomb::set_rlimits() {
  if(m_nofiles) set_rlimit(RLIMIT_NOFILE, m_nofiles);
}

int Honeycomb::set_rlimit(const int res, const rlim_t limit) {
  struct rlimit lmt = { limit, limit };
  if (setrlimit(res, &lmt)) {
    fprintf(stderr, "Could not set resource limit: %d\n", res);
    return(-1);
  }
  return 0;
}

/*---------------------------- UTILS ------------------------------------*/
int Honeycomb::copy_deps(const std::string & root_path, const std::string & file_path) {
  WorkerBee b;

  string_set s_executables;
  s_executables.insert("/bin/ls");
  s_executables.insert("/bin/bash");
  s_executables.insert("/usr/bin/whoami");
  s_executables.insert("ruby");

  string_set s_dirs;
  s_dirs.insert("/opt");

  printf("-- building chroot: %s\n", root_path.c_str());
  b.build_chroot(root_path, m_user, m_group, s_executables, s_dirs);

  return 0; 
}

const char *DEV_RANDOM = "/dev/urandom";
uid_t Honeycomb::random_uid() {
  uid_t u;
  for (unsigned char i = 0; i < 10; i++) {
    int rndm = open(DEV_RANDOM, O_RDONLY);
    if (sizeof(u) != read(rndm, reinterpret_cast<char *>(&u), sizeof(u))) {
      continue;
    }
    close(rndm);

    if (u > 0xFFFF) {
      return u;
    }
  }

  DEBUG_MSG("Could not generate a good random UID after 10 attempts. Bummer!");
  return(-1);
}
const char * const Honeycomb::to_string(long long int n, unsigned char base) {
  static char bfr[32];
  const char * frmt = "%lld";
  if (16 == base) {frmt = "%llx";} else if (8 == base) {frmt = "%llo";}
  snprintf(bfr, sizeof(bfr) / sizeof(bfr[0]), frmt, n);
  return bfr;
}

/*---------------------------- Permissions ------------------------------------*/

int Honeycomb::temp_drop() {
  DEBUG_MSG("Dropping into '%d' user\n", m_user);
  if (setresgid(-1, m_user, getegid()) || setresuid(-1, m_user, geteuid()) || getegid() != m_user || geteuid() != m_user ) {
    fprintf(stderr, "Could not drop privileges temporarily to %d: %s\n", m_user, ::strerror(errno));
    return -1; // we are in the fork
  }
  return 0;
}
int Honeycomb::perm_drop() {
  uid_t ruid, euid, suid;
  gid_t rgid, egid, sgid;

  if (setresgid(m_user, m_user, m_user) || setresuid(m_user, m_user, m_user) || getresgid(&rgid, &egid, &sgid)
      || getresuid(&ruid, &euid, &suid) || rgid != m_user || egid != m_user || sgid != m_user
      || ruid != m_user || euid != m_user || suid != m_user || getegid() != m_user || geteuid() != m_user ) {
    fprintf(stderr,"\nError setting user to %u\n",m_user);
    return -1;
  }
  return 0;
}
int Honeycomb::restore_perms() {
  uid_t ruid, euid, suid;
  gid_t rgid, egid, sgid;

  if (getresgid(&rgid, &egid, &sgid) || getresuid(&ruid, &euid, &suid) || setresuid(-1, suid, -1) || setresgid(-1, sgid, -1)
      || geteuid() != suid || getegid() != sgid ) {
        fprintf(stderr, "Could not drop privileges temporarily to %d: %s\n", m_user, ::strerror(errno));
        return -1; // we are in the fork
      }
  return 0;
}

