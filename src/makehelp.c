
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>

char *replace(char *string, char *oldie, char *newbie)
{
  static char newstring[1024] = "";
  int str_index, newstr_index, oldie_index, end, new_len, old_len, cpy_len;
  char *c = NULL;

  if (string == NULL) return "";
  if ((c = (char *) strstr(string, oldie)) == NULL) return string;
  new_len = strlen(newbie);
  old_len = strlen(oldie);
  end = strlen(string) - old_len;
  oldie_index = c - string;
  newstr_index = 0;
  str_index = 0;
  while(str_index <= end && c != NULL) {
    cpy_len = oldie_index-str_index;
    strncpy(newstring + newstr_index, string + str_index, cpy_len);
    newstr_index += cpy_len;
    str_index += cpy_len;
    strcpy(newstring + newstr_index, newbie);
    newstr_index += new_len;
    str_index += old_len;
    if((c = (char *) strstr(string + str_index, oldie)) != NULL)
     oldie_index = c - string;
  }
  strcpy(newstring + newstr_index, string + str_index);
  return (newstring);
}


char *step_thru_file(FILE *fd)
{
  char tempBuf[1024] = "";
  char *retStr = NULL;

  if (fd == NULL) {
    return NULL;
  }
  retStr = NULL;
  while (!feof(fd)) {
    fgets(tempBuf, sizeof(tempBuf), fd);
    if (!feof(fd)) {
      if (retStr == NULL) {
        retStr = (char *) calloc(1, strlen(tempBuf) + 2);
        strcpy(retStr, tempBuf);
      } else {
        retStr = (char *) realloc(retStr, strlen(retStr) + strlen(tempBuf));
        strcat(retStr, tempBuf);
      }
      if (retStr[strlen(retStr)-1] == '\n') {
        retStr[strlen(retStr)-1] = 0;
        break;
      }
    }
  }
  return retStr;
}

char *newsplit(char **rest)
{
  register char *o, *r;

  if (!rest)
    return *rest = "";
  o = *rest;
  while (*o == ' ')
    o++;
  r = o;
  while (*o && (*o != ' '))
    o++;
  if (*o)
    *o++ = 0;
  *rest = o;
  return r;
}

int skipline (char *line, int *skip) {
  static int multi = 0;
  if ((!strncmp(line, "//", 2))) {
    (*skip)++;
  } else if ( (strstr(line, "/*")) && (strstr(line, "*/")) ) {
    multi = 0;
    (*skip)++;
  } else if ( (strstr(line, "/*")) ) {
    (*skip)++;
    multi = 1;
  } else if ( (strstr(line, "*/")) ) {
    multi = 0;
  } else {
    if (!multi) (*skip) = 0;
  }
  return (*skip);
}

#define type(hub, leaf) (hub ? 1 : (leaf ? 2 : 0))

int parse_help(char *infile, char *outfile) {
  FILE *in = NULL, *out = NULL;
  char *buffer = NULL, *cmd = NULL;
  int skip = 0, line = 0, leaf = 0, hub = 0;

  if (!(in = fopen(infile, "r"))) {
    printf("Error: Cannot open '%s' for reading\n", infile);
    return 1;
  }
  if (!(out = fopen(outfile, "w"))) {
    printf("Error: Cannot open '%s' for writing\n", outfile);
    return 1;
  }
  printf("Parsing help file '%s'", infile);
  fprintf(out, "/* DO NOT EDIT THIS FILE, EDIT misc/help.txt INSTEAD */\n#ifndef HELP_H\n\
#define HELP_H\n\
#define STR(x) x\n\
typedef struct { \n\
  int type; \n\
  char *cmd; \n\
  int garble; \n\
  char *desc; \n\
} help_t; \n\
\n\
help_t help[] = \n\
{ \n");
  while ((!feof(in)) && ((buffer = step_thru_file(in)) != NULL) ) {
    line++;
    if ((*buffer)) {
      if (strchr(buffer, '\n')) *(char*)strchr(buffer, '\n') = 0;
      if ((skipline(buffer, &skip))) continue;
      if (buffer[0] == ':') { /* New cmd */
        char *ifdef = (char *) calloc(1, strlen(buffer) + 1), *p = NULL;
        int cl = 0, doleaf = 0, dohub = 0;

        buffer++;
        strcpy(ifdef, buffer);
        p = strchr(ifdef, ':');
        *p = 0;
        if (ifdef && ifdef[0]) {
          if (!strcasecmp(ifdef, "leaf")) {
            if (hub) { cl = 1; hub = 0; }
            if (!leaf) {
              doleaf = leaf = 1;
            }
          } else if (!strcasecmp(ifdef, "hub")) {
            if (leaf) { cl = 1; hub = 0; }
            if (!hub) {
              dohub = hub = 1;
            }
          }
        } else { if (leaf || hub) { cl = 1; } leaf = 0; hub = 0;  }

        if (cmd && cmd[0]) {		/* CLOSE LAST CMD */
          if (strchr(cmd, ':'))		/* garbled */
            fprintf(out,"\")},\n");
          else
            fprintf(out,"\"},\n");
//          if (cl) { cl = 0; fprintf(out, "#endif\n"); }
//          if (dohub) { dohub = 0; fprintf(out, "#ifdef HUB\n"); }
//          else if (doleaf) { doleaf = 0; fprintf(out, "#ifdef LEAF\n"); }
          free(cmd);
        }
        p = strchr(buffer, ':');
        p++;
        if (strcmp(p, "end")) {		/* NEXT CMD */
          cmd = (char *) calloc(1, strlen(p) + 1);

          strcpy(cmd, p);
          printf(".");
//          if (dohub) { dohub = 0; fprintf(out, "#ifdef HUB\n"); }
//          else if (doleaf) { doleaf = 0; fprintf(out, "#ifdef LEAF\n"); }
          if (strchr(cmd, ':')) {
            char *p2 = NULL, *cmdn = (char *) calloc(1,strlen(cmd) + 1);

            strcpy(cmdn, cmd);
            p2 = strchr(cmdn, ':');
            *p2 = 0;
            fprintf(out, "  {%d, \"%s\", STR(\"", type(dohub, doleaf), cmdn);
          } else
            fprintf(out, "  {%d, \"%s\", 0, \"", type(dohub, doleaf), cmd);
        } else {			/* END */
          fprintf(out, "  {0, NULL, 0, NULL}\n};\n");
        }
      } else {				/* CMD HELP INFO */
        fprintf(out, "%s\\n", replace(buffer, "\"", "\\\""));
      }
    }
    buffer = NULL;
  }
  fprintf(out, "#endif /* HELP_H */\n");
  printf(" Success\n");
  if (in) fclose(in);
  if (out) fclose(out);
  return 0;
}

int main(int argc, char **argv) {
  char *in = NULL, *out = NULL;
  int ret = 0;

  if (argc < 3) return 1;
  in = (char *) calloc(1, strlen(argv[1]) + 1);
  strcpy(in, argv[1]);
  out = (char *) calloc(1, strlen(argv[2]) + 1);
  strcpy(out, argv[2]);
  ret = parse_help(in, out);
  free(in);
  free(out);
  return ret;
}
