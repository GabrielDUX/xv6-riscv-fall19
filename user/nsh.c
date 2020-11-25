#include "kernel/types.h"
#include "user/user.h"
#include "kernel/fcntl.h"

// 定义命令类型,参考sh.c
#define EXEC  1
#define REDIR 2
#define PIPE  3
#define LIST  4
#define BACK  5

// 定义最大参数长度
#define MAXARGS 10

char cmd_buf[1024];
char *cmd_p = cmd_buf;
char whitespace[] = " \t\r\n\v";
char symbols[] = "<|>&;()";

// 以下结构体定义参考sh.c
// 基本命令类型
struct cmd
{
    int type;
};

// 单命令，直接执行
struct execcmd
{
    int type;
    char *argv[MAXARGS];
    char *eargv[MAXARGS];
};

// 重定向命令
struct redircmd
{
    int type;
    struct cmd *cmd;
    char *file;
    char *efile;
    int mode;
    int fd;
};

// 管道命令，分别执行左右两个命令
// 再用管道将输出与输入连接
struct pipecmd
{
    int type;
    struct cmd *left;
    struct cmd *right;
};


struct cmd *pipecmd(struct cmd *left, struct cmd *right){
    struct pipecmd *cmd;
    
    cmd = (struct pipecmd*)cmd_p;
    cmd_p += sizeof(*cmd);
    memset(cmd, 0, sizeof(cmd));
    cmd->type = PIPE;
    cmd->left = left;
    cmd->right = right;
    return (struct cmd*)cmd;
}

struct cmd* execcmd(void)
{
  struct execcmd *cmd;

  cmd = (struct execcmd*)cmd_p;
  cmd_p += sizeof(*cmd);
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = EXEC;
  return (struct cmd*)cmd;
}

struct cmd* redircmd(struct cmd *subcmd, char *file, char *efile, int mode,int fd){
    struct redircmd *cmd;

    cmd = (struct redircmd*)cmd_p;
    cmd_p += sizeof(*cmd);
    memset(cmd, 0, sizeof(cmd));
    cmd->type = REDIR;
    cmd->cmd = subcmd;
    cmd->file = file;
    cmd->efile = efile;
    cmd->mode = mode;
    cmd->fd = fd;
    return (struct cmd*)cmd;
}



// 找到第一个非空格的字符，若为toks则返回真
int peek(char **ps,char *es, char *toks){
    char *s;

    s = *ps;
    while(s < es && strchr(whitespace, *s))
    s++;
    *ps = s;
    return *s && strchr(toks, *s);
}


// 解析ps位置的字符。如果是symbol，则返回symbol；
// 如果是字母，则返回'a',并定位到下一个symbol的位置（ps)
int
gettoken(char **ps, char *es, char **q, char **eq)
{
  char *s;
  int ret;

  s = *ps;
  while(s < es && strchr(whitespace, *s)) //找到第一个非空格的字符
    s++;
  if(q)  
    *q = s;
  ret = *s;
  switch(*s){
  case 0:
    break;
  case '|':
  case '<':
  case '>':
    s++;
    break;
  default:
    ret = 'a';
    while(s < es && !strchr(whitespace, *s) && !strchr(symbols, *s)) //如果是字母，就寻找到下一个字母
      s++;
    break;
  }
  if(eq)
    *eq = s; //实际上这个位置指向的是该字母串末尾的下一个位置

  while(s < es && strchr(whitespace, *s))  //继续寻找到下一个非空格字符
    s++;
  *ps = s;
  return ret;
}

struct cmd *parseredirs(struct cmd*, char**, char*);
struct cmd *parseline(char**, char*);
struct cmd *parseexec(char**, char*);
struct cmd *parsepipe(char**, char*);
struct cmd *nulterminate(struct cmd*);

struct cmd *parseexec(char **ps, char *es)
{
    char *q, *eq;
    int tok, argc;
    struct execcmd *cmd;
    struct cmd *ret;

    ret = execcmd();
    cmd = (struct execcmd*)ret;

    argc = 0;
    ret = parseredirs(ret, ps, es);
    while (!peek(ps, es,"|"))
    {
        if((tok=gettoken(ps,es,&q,&eq)) == 0) //达到末尾
            break;
        // fprintf(2,"tok:%c\n",tok);
        if(tok!='a') //不是字母，则语法错误
        {
            fprintf(2,"syntax");
            exit(-1);
        }
        cmd->argv[argc] = q;
        cmd->eargv[argc] = eq;
        argc++;
        if(argc >= MAXARGS){
            fprintf(2,"too many args");
            exit(-1);
        }
        ret = parseredirs(ret, ps, es);
    }
    cmd->argv[argc] = 0;
    cmd->eargv[argc] = 0;
    return ret;
};

struct cmd *parseredirs(struct cmd *cmd,char **ps, char *es){
    int tok;
    char *q, *eq;
    
    while(peek(ps,es,"<>")){ //定位到重定向符
        tok = gettoken(ps,es,0,0);
        // fprintf(2,"%c",tok);
        if(gettoken(ps,es,&q,&eq)!='a') {
            fprintf(2,"missing file for redirection");
            exit(-1);
        }
        switch (tok)
        {
        case '<':
            cmd = redircmd(cmd, q, eq, O_RDONLY, 0);
            break;
        case '>':
            cmd = redircmd(cmd, q, eq, O_WRONLY|O_CREATE, 1);
            break;
        default:
            break;
        }
    }


    return cmd;
}


struct cmd* parsepipe(char **ps, char *es)
{
    struct cmd *cmd;
    cmd = parseexec(ps,es);
    if(peek(ps,es,"|")){
        gettoken(ps,es,0,0);
        cmd = pipecmd(cmd, parsepipe(ps,es));
    }
    return cmd;
};


struct cmd* parseline(char **ps, char *es){
    struct cmd *cmd;

    cmd = parsepipe(ps,es);

    return cmd;
}


// 解析命令,返回cmd指针
struct cmd* parsecmd(char *s){
    char *es;
    struct cmd *cmd;

    es = s + strlen(s);
    cmd = parseline(&s,es);
    peek(&s, es, "");
    if(s != es){
        fprintf(2,"leftovers: %s\n", s);
        fprintf(2,"syntax\n");
        exit(-1);
    }
    nulterminate(cmd);

    return cmd;


}


// 根据输入的命令类型执行命令，参考sh.c
void runcmd(struct cmd *cmd) {
    int p[2];
    struct execcmd *ecmd;
    struct pipecmd *pcmd;
    struct redircmd *rcmd;

    if(cmd == 0)   //没有输入
        exit(-1);

    switch (cmd->type)
    {
    case EXEC:
        ecmd = (struct execcmd*)cmd;
        // fprintf(2,"exec cmd type: %s\n",ecmd->argv[0]);
        if(ecmd->argv[0] == 0)
            exit(-1);
        exec(ecmd->argv[0], ecmd->argv);
        fprintf(2,"exec %s failed\n", ecmd->argv[0]); //exec failed,print error meassage
        break;
    
    case REDIR:
        rcmd = (struct redircmd*)cmd;
        close(rcmd->fd); 
        if(open(rcmd->file, rcmd->mode)<0) { //open file
            fprintf(2, "open %s failed\n", rcmd->file);
            exit(-1);
        }
        runcmd(rcmd->cmd);
        break;
    
    case PIPE:
        pcmd = (struct pipecmd*)cmd;
        if(pipe(p)<0) {
            fprintf(2,"Create pipe failed!\n");
            exit(-1);
        }
        if(fork() == 0) { //执行左命令，并通过管道将结果（标准输出）传送到右命令
            close(1);
            dup(p[1]);
            close(p[0]);
            close(p[1]);
            runcmd(pcmd->left);
        }
        if(fork()==0) { //从标准输入stdin读取参数
            close(0);
            dup(p[0]);
            close(p[0]);
            close(p[1]);
            runcmd(pcmd->right);
        }
        close(p[0]);
        close(p[1]); //close father's pipe;
        wait(0); //等待子进程结束（等待执行完成）
        wait(0);
        break;

    default:
        break;
    }
    exit(0);
}

// 获取命令，存入buf中，参考sh.c实现
int getcmd(char *buf, int buf_size)
{
    fprintf(2,"@ ");
    memset(buf, 0, buf_size); //init buf
    gets(buf, buf_size);
    if(buf[0] == 0)  //EOF
        return -1;
    return 0;
}

// 将得到的参数划分
struct cmd* nulterminate(struct cmd *cmd){
    int i;
    struct execcmd *ecmd;
    struct pipecmd *pcmd;
    struct redircmd *rcmd;

    if(cmd == 0)  //无效命令
        return 0;
    
    switch(cmd->type){
        case EXEC:
        ecmd = (struct execcmd*)cmd;
        for(i=0; ecmd->argv[i]; i++)
            *ecmd->eargv[i]=0;
        break;

        case REDIR:
            rcmd = (struct redircmd*)cmd;
            nulterminate(rcmd->cmd);
            *rcmd->efile = 0;
            break;

        case PIPE:
        pcmd = (struct pipecmd*)cmd;
        nulterminate(pcmd->left);
        nulterminate(pcmd->right);
        break;
    }
    return cmd;
}



int 
main () {
    static char buf[100];

    while(getcmd(buf,sizeof(buf)) >= 0) { //read cmd from keyboard
        if(buf[0]=='c' && buf[1]=='d' && buf[2]==' '){
            buf[strlen(buf)-1] = 0;  // chop \n
            if(chdir(buf+3) < 0)
                fprintf(2, "cannot cd %s\n", buf+3);
            continue;
        }

        if(fork() == 0) {
            struct cmd *cmd = parsecmd(buf);
            runcmd(cmd);
        }
        wait(0);  //等待子进程解析并执行命令
    }
    exit(0);
}


