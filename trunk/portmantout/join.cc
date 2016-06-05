// Joins particles generated by particles.exe; see paper.pdf.

#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <deque>

#include "util.h"
#include "timer.h"
#include "threadutil.h"
#include "arcfour.h"
#include "randutil.h"
#include "base/logging.h"
#include "base/stringprintf.h"

#include "makejoin.h"

using namespace std;

std::mutex print_mutex;
#define Printf(fmt, ...) do {		\
  MutexLock Printf_ml(&print_mutex);		\
  printf(fmt, ##__VA_ARGS__);			\
  } while (0);

int main () {
  vector<string> dict = Util::ReadFileToLines("wordlist.asc");
  printf("%d words.\n", (int)dict.size());
  vector<string> particles = Util::ReadFileToLines("particles.txt");
  printf("%d particles.\n", (int)particles.size());
  ArcFour rc("portmantout");

  int total_letters = 0;
  for (const string &w : dict) {
    total_letters += w.size();
  }
  printf("%d total letters.\n", total_letters);
  

  vector<vector<Path>> matrix = MakeJoin(dict);

  auto SaveTable = [&matrix](const string &filename) {
    {
      FILE *f = fopen(StringPrintf("%s.html", filename.c_str()).c_str(), "wb");
      fprintf(f, "<!doctype html>\n"
	      "<style> td { padding: 0 4px } body { font: 13px Verdana } </style>\n"
	      "<table>");

      fprintf(f, "<tr><td>&nbsp;</td> ");
      for (int i = 0; i < 26; i++)
	fprintf(f, "<td>%c</td>", 'a' + i);
      fprintf(f, " </tr>\n");

      for (int src = 0; src < 26; src++) {
	fprintf(f, "<tr><td>%c</td> ", 'a' + src);
	for (int dst = 0; dst < 26; dst++) {
	  const Path &p = matrix[src][dst];
	  if (p.has) {
	    fprintf(f, "<td>%s</td>", p.word.c_str());
	  } else {
	    fprintf(f, "<td>&mdash;</td>");
	  }
	}
	fprintf(f, " </tr>\n");
      }

      fprintf(f, "</table>\n");
      fclose(f);
    }

    {
      FILE *f = fopen(StringPrintf("%s.tex", filename.c_str()).c_str(), "wb");

      auto Cols = [&f, &matrix](int col_start, int col_end) {
	fprintf(f, "\\noindent \\begin{tabular}{c");
	for (int i = col_start; i < col_end; i++)
	  fprintf(f, "l");
	fprintf(f, "}\n");

	fprintf(f, " --- ");
	for (int i = col_start; i < col_end; i++)
	  fprintf(f, "& {\\bf %c} ", 'a' + i);
	fprintf(f, "\\\\\n");

	for (int src = 0; src < 26; src++) {
	  fprintf(f, "{\\bf %c} ", 'a' + src);
	  for (int dst = col_start; dst < col_end; dst++) {
	    const Path &p = matrix[src][dst];
	    if (p.has) {
	      fprintf(f, " & %s ", p.word.c_str());
	    } else {
	      fprintf(f, " & m--- ");
	    }
	  }
	  fprintf(f, " \\\\\n");
	}

	fprintf(f, "\\end{tabular}");
      };

      Cols(0, 13);
      fprintf(f, "\n\n");
      Cols(13, 26);
      
      fclose(f);
    }
  };


  SaveTable("table-two");
  
  string portmantout = particles[0];
  for (int i = 1; i < particles.size(); i++) {
    CHECK(portmantout.size() > 0);
    const string &next = particles[i];
    CHECK(next.size() > 0);
    int src = portmantout[portmantout.size() - 1] - 'a';
    int dst = next[0] - 'a';
    CHECK(matrix[src][dst].has);
    const string &bridge = matrix[src][dst].word;

    // Chop one char.
    portmantout.resize(portmantout.size() - 1);
    portmantout += bridge;
    // And again.
    portmantout.resize(portmantout.size() - 1);
    portmantout += next;
  }
  
  printf("All done! The portmantout is assembled, of size %d. (%.1f%% efficiency)\n",
	 (int)portmantout.size(),
	 (portmantout.size() * 100.0) / total_letters);
  {
    FILE *f = fopen("portmantout.txt", "wb");
    fprintf(f, "%s\n", portmantout.c_str());
    fclose(f);
  }

  {
    FILE *f = fopen("portmantout.tex", "wb");
    int col = 0;
    static constexpr int MAX_COLUMN = 255;
    for (char c : portmantout) {
      fprintf(f, "%c", c);
      col++;
      if (col == MAX_COLUMN) {
	fprintf(f, "\n");
	col = 0;
      }
    }
    fprintf(f, "\n");
    fclose(f);
  }

  return 0;
}