#include <vector>
#include <string>
#include <set>
#include <memory>

#include <cstdio>
#include <cstdlib>

#include "pftwo.h"

#include "../fceulib/emulator.h"
#include "../fceulib/simplefm2.h"
#include "../cc-lib/sdl/sdlutil.h"
#include "../cc-lib/util.h"
#include "../cc-lib/arcfour.h"
#include "../cc-lib/textsvg.h"
#include "../cc-lib/stb_image.h"
#include "../cc-lib/sdl/chars.h"
#include "../cc-lib/sdl/font.h"
#include "../cc-lib/heap.h"

#include "atom7ic.h"

#include "motifs.h"
#include "weighted-objectives.h"

#include "SDL.h"

#include "problem-twoplayer.h"

#define WIDTH 1920
#define HEIGHT 1080

// "Functor"
using Problem = TwoPlayerProblem;
using Worker = Problem::Worker;

struct PF2;
struct WorkThread;
struct UIThread;

std::mutex print_mutex;
#define Printf(fmt, ...) do {			\
    MutexLock Printf_ml(&print_mutex);		\
    printf(fmt, ##__VA_ARGS__);			\
  } while (0)

// TODO(twm): use shared_mutex when available
static bool should_die = false;
static mutex should_die_m;

// assumes ARGB, surfaces exactly the same size, etc.
static void CopyARGB(const vector<uint8> &argb, SDL_Surface *surface) {
  // int bpp = surface->format->BytesPerPixel;
  Uint8 * p = (Uint8 *)surface->pixels;
  memcpy(p, argb.data(), surface->w * surface->h * 4);
}

static void BlitARGB(const vector<uint8> &argb, int w, int h,
		     int x, int y, SDL_Surface *surface) {
  for (int i = 0; i < h; i++) {
    int yy = y + i;
    Uint8 *p = (Uint8 *)surface->pixels +
      (surface->w * 4 * yy) + x * 4;
    memcpy(p, argb.data() + (i * w * 4), w * 4);
  }
}

static inline constexpr uint8 Mix4(uint8 v1, uint8 v2, uint8 v3, uint8 v4) {
  return (uint8)(((uint32)v1 + (uint32)v2 + (uint32)v3 + (uint32)v4) >> 2);
}

static void HalveARGB(const vector<uint8> &argb, int width, int height,
		      SDL_Surface *surface) {
  // PERF
  const int halfwidth = width >> 1;
  const int halfheight = height >> 1;
  vector<uint8> argb_half;
  argb_half.resize(halfwidth * halfheight * 4);
  for (int y = 0; y < halfheight; y++) {
    for (int x = 0; x < halfwidth; x++) {
      #define PIXEL(i) \
	argb_half[(y * halfwidth + x) * 4 + (i)] =	\
	  Mix4(argb[(y * 2 * width + x * 2) * 4 + (i)], \
	       argb[(y * 2 * width + x * 2 + 1) * 4 + (i)], \
	       argb[((y * 2 + 1) * width + x * 2) * 4 + (i)],	\
	       argb[((y * 2 + 1) * width + x * 2 + 1) * 4 + (i)])
      PIXEL(0);
      PIXEL(1);
      PIXEL(2);
      PIXEL(3);
      #undef PIXEL
    }
  }
  CopyARGB(argb_half, surface);
}

static void BlitARGBHalf(const vector<uint8> &argb, int width, int height,
			 int xpos, int ypos,
			 SDL_Surface *surface) {
  const int halfwidth = width >> 1;
  const int halfheight = height >> 1;
  // argb_half.resize(halfwidth * halfheight * 4);
  for (int y = 0; y < halfheight; y++) {
    int yy = ypos + y;
    Uint8 *p = (Uint8 *)surface->pixels +
      (surface->w * 4 * yy) + xpos * 4;

    for (int x = 0; x < halfwidth; x++) {
      #define PIXEL(i) \
	*p = \
	  Mix4(argb[(y * 2 * width + x * 2) * 4 + (i)], \
	       argb[(y * 2 * width + x * 2 + 1) * 4 + (i)], \
	       argb[((y * 2 + 1) * width + x * 2) * 4 + (i)],	\
	       argb[((y * 2 + 1) * width + x * 2 + 1) * 4 + (i)]); \
	p++
      PIXEL(0);
      PIXEL(1);
      PIXEL(2);
      PIXEL(3);
      #undef PIXEL
    }
  }
  // CopyARGB(argb_half, surface);
}

// Tree of state exploration.
struct Tree {
  using Seq = vector<Problem::Input>;
  static constexpr int UPDATE_FREQUENCY = 1000;

  struct Node {
    Node(std::shared_ptr<Problem::State> state, Node *parent) :
      state(state), parent(parent) {}
    // Optional, except at the root. This can be recreated by
    // replaying the path from some ancestor.
    std::shared_ptr<Problem::State> state;

    // Only null for the root.
    Node *parent = nullptr;

    // Child nodes. Currently, no guarantee that these don't
    // share prefixes, but there should not be duplicates.
    map<Seq, Node *> children;

    // For heap. Not all nodes will be in the heap.
    int location = -1;
  };

  Tree(double score, const Problem::State &state) {
    root = new Node(std::make_shared<Problem::State>(state), nullptr);
    heap.Insert(-score, root);
  }

  // Tree prioritized by negation of score at current epoch. Not all
  // nodes are guaranteed to be present. Negation is used so that the
  // minimum node is actually the node with the best score.
  Heap<double, Node> heap;

  Node *root = nullptr;
  int steps_until_update = UPDATE_FREQUENCY;
  // If this is true, a thread is updating the tree. It does not
  // necessarily hold the lock, because some calculations can be
  // done without the tree (like sorting observations). If set,
  // another thread should avoid also beginning an update.
  bool update_in_progress = false;
  int64 num_nodes = 0;
};


struct PF2 {
  std::unique_ptr<Motifs> motifs;

  // Should not be modified after initialization.
  std::unique_ptr<Problem> problem;

  // Approximately one per CPU. Created at startup and lives
  // until should_die becomes true. Pointers owned by PF2.
  vector<WorkThread *> workers;
  int num_workers = 0;

  UIThread *ui_thread = nullptr;

  // Initialized by one of the workers with the post-warmup
  // state.
  Tree *tree = nullptr;
  // Coarse locking.
  std::mutex tree_m;


  PF2() {
    map<string, string> config = Util::ReadFileToMap("config.txt");
    if (config.empty()) {
      fprintf(stderr, "Missing config.txt.\n");
      abort();
    }

    num_workers = atoi(config["workers"].c_str());
    if (num_workers <= 0) {
      fprintf(stderr, "Warning: In config, 'workers' should be set >0.\n");
      num_workers = 1;
    }

    problem.reset(new Problem(config));
    printf("Problem at %p\n", problem.get());
  }

  void StartThreads();
  void DestroyThreads();

  void Play() {
    // Doesn't return until UI thread exits.
    StartThreads();

    DestroyThreads();
  }
};

struct WorkThread {
  using Node = Tree::Node;
  WorkThread(PF2 *pftwo, int id) : pftwo(pftwo), id(id),
				   rc(StringPrintf("worker_%d", id)),
				   worker(pftwo->problem->CreateWorker()),
				   th{&WorkThread::Run, this} {}

  // Get the root node of the tree and associated state (which must be
  // present). Used during initialization.
  pair<Node *, shared_ptr<Problem::State>> GetRoot() {
    MutexLock ml(&pftwo->tree_m);
    Node *n = pftwo->tree->root;
    return {n, n->state};
  }
  
  // XXX: This is a silly policy. Use a heap or something...
  pair<Node *, shared_ptr<Problem::State>> Descend(Node *n) {
    MutexLock ml(&pftwo->tree_m);
    for (;;) {
      CHECK(n != nullptr);
      // If a leaf, it must have a state, and we must return it.
      if (n->children.empty()) {
	CHECK(n->state.get() != nullptr);
	return {n, n->state};
      }

      if (n->state.get() != nullptr && rc.Byte() < 16)
	return {n, n->state};

      int idx = RandTo(&rc, n->children.size());
      auto it = n->children.begin();
      while (idx--) ++it;
      n = it->second;
    }
  }

  // Move up the tree a random amount.
  Node *Ascend(Node *n) {
    MutexLock ml(&pftwo->tree_m);
    while (n->parent != nullptr && rc.Byte() < 128) {
      n = n->parent;
    }
    return n;
  }

  pair<Node *, shared_ptr<Problem::State>> FindNodeToExtend(Node *n) {
    MutexLock ml(&pftwo->tree_m);

    // Simple policy: 50% chance of switching to the best node
    // in the heap; 50% chance of staying with the current node.
    if (n->state.get() == nullptr || rc.Byte() < 128) {
      const int size = pftwo->tree->heap.Size();
      CHECK(size > 0) << "Heap should always have root, at least.";

      // Weight towards the top of the heap, but allow sampling
      // deeper.
      int idx = 0;
      while (idx < size - 1 && rc.Byte() < 128 + 64) {
	idx++;
      }

      Heap<double, Node>::Cell cell = pftwo->tree->heap.GetByIndex(idx);
      CHECK(cell.value->state.get() != nullptr);
      return {cell.value, cell.value->state};
    } else {
      return {n, n->state};
    }
  }
  
  Node *ExtendNode(Node *n,
		   const vector<Problem::Input> &step,
		   shared_ptr<Problem::State> newstate) {
    CHECK(n != nullptr);
    const double newscore = pftwo->problem->Score(*newstate);
    Node *child = new Node(newstate, n);
    MutexLock ml(&pftwo->tree_m);
    auto res = n->children.insert({step, child});
    if (!res.second) {
      // By dumb luck, we already have a node with this
      // exact path. We don't allow replacing it.
      delete child;
      // Maintain the invariant that the worker is at the
      // state of the returned node.
      Node *ch = res.first->second;
      // Just find any descendant with a state. Eventually
      // we get to a leaf, which must have a state.
      for (;;) {
	CHECK(ch != nullptr);
	if (ch->state.get() != nullptr) {
	  worker->Restore(*ch->state);
	  return ch;
	}
	CHECK(!ch->children.empty());
	ch = ch->children.begin()->second;
      }
    } else {
      pftwo->tree->num_nodes++;
      pftwo->tree->heap.Insert(-newscore, child);
      CHECK(child->location != -1);
      return child;
    }
  }

  void InitializeTree() {
    MutexLock ml(&pftwo->tree_m);
    if (pftwo->tree == nullptr) {
      // I won the race!
      printf("Initialize tree...\n");
      Problem::State s = worker->Save();
      pftwo->tree = new Tree(pftwo->problem->Score(s), s);
    }
  }


  void MaybeUpdateTree() {
    Tree *tree = pftwo->tree;
    {
      MutexLock ml(&pftwo->tree_m);
      // No use in decrementing update counter -- when we
      // finish we reset it to the max value.
      if (tree->update_in_progress)
	return;

      if (tree->steps_until_update == 0) {
	tree->steps_until_update = Tree::UPDATE_FREQUENCY;
	tree->update_in_progress = true;
	// Enter fixup below.
      } else {
	tree->steps_until_update--;
	return;
      }
    }

    // This is run every UPDATE_FREQUENCY calls, in some
    // arbitrary worker thread. We don't hold any locks right
    // now, but only one thread will enter this section at
    // a time because of the update_in_progress flag.
    worker->SetStatus("Tree: Commit observations");

    pftwo->problem->Commit();

    // Re-build heap.
    worker->SetStatus("Tree: Reheap");
    {
      MutexLock ml(&pftwo->tree_m);
      printf("Reheap!");
      // tree->heap.Clear();

      std::function<void(Node *)> ReHeapRec =
	[this, tree, &ReHeapRec](Node *n) {
	// Act on the node if it's in the heap. 
	if (n->location != -1) {
	  // We need a state to rescore it.
	  if (n->state.get() != nullptr) {
	    const double new_score = pftwo->problem->Score(*n->state);
	    // Note negation of score so that bigger real scores
	    // are more minimum for the heap ordering.
	    tree->heap.AdjustPriority(n, -new_score);
	    CHECK(n->location != -1);
	  } else {
	    // So if no state, remove it.
	    tree->heap.Delete(n);
	    CHECK(n->location == -1);
	  }
	}
	for (pair<const Tree::Seq, Node *> &child : n->children) {
	  ReHeapRec(child.second);
	}
      };
      ReHeapRec(tree->root);
      printf("Done!");
    }


    {
      MutexLock ml(&pftwo->tree_m);
      tree->update_in_progress = false;
    }
  }

 
  void Run() {
    worker->Init();

    InitializeTree();


    Node *last = nullptr;
    // Initialize the worker so that its previous state is the root.
    // With the current setup it should already be in this state, but
    // it's inexpensive to explicitly establish the invariant.
    {
      worker->SetStatus("Load root");
      shared_ptr<Problem::State> root_state;
      std::tie(last, root_state) = GetRoot();
      CHECK(last != nullptr);
      CHECK(root_state.get() != nullptr);
      worker->Restore(*root_state);
    }
    
    static constexpr int FRAMES_PER_STEP = 400;

    // XXX don't make this finite
    worker->SetDenom(50000 * FRAMES_PER_STEP);
    for (int i = 0; i < 50000; i++) {
      // Starting this loop, the worker is in some problem state that
      // corresponds to the node 'last' in the tree. We'll extend this
      // node or find a different one.
      worker->SetNumer(i * FRAMES_PER_STEP);
      worker->SetStatus("Find start node");
      Node *next;
      shared_ptr<Problem::State> state;
      std::tie(next, state) = FindNodeToExtend(last);
      active.store(next, std::memory_order_relaxed);
      
      // PERF skip this if it's the same as last?
      worker->SetStatus("Load");
      CHECK(next != nullptr);
      CHECK(state.get() != nullptr);
      worker->Restore(*state);

      worker->SetStatus("Make inputs");
      vector<Problem::Input> step;
      step.reserve(40);
      for (int num_left = FRAMES_PER_STEP; num_left--;)
	step.push_back(worker->RandomInput(&rc));

      worker->SetStatus("Execute inputs");
      for (const Problem::Input &input : step)
	worker->Exec(input);

      worker->SetStatus("Observe state");
      worker->Observe();

      worker->SetStatus("Extend tree");
      shared_ptr<Problem::State> newstate{
	new Problem::State(worker->Save())};
      last = ExtendNode(next, step, newstate);

      MaybeUpdateTree();

      worker->SetStatus("Check for death");
      if (ReadWithLock(&should_die_m, &should_die)) {
	worker->SetStatus("Die");
	return;
      }
    }
  }

  // Expects that should_die is true or will become true.
  ~WorkThread() {
    Printf("Worker %d shutting down...\n", id);
    th.join();
    Printf("Worker %d done.\n", id);
  }

  Worker *GetWorker() const { return worker; }

  bool IsActiveNode(const Node *n) {
    return n == active.load(std::memory_order_relaxed);
  }
   
private:
  // Just used for IsActiveNode.
  std::atomic<Node *> active{nullptr};

  PF2 *pftwo = nullptr;
  const int id = 0;
  // Distinct private stream.
  ArcFour rc;
  Problem::Worker *worker = nullptr;
  std::thread th;
};

// Note: This is really the main thread. SDL really doesn't
// like being called outside the main thread, even if it's
// exclusive.
struct UIThread {
  UIThread(PF2 *pftwo) : pftwo(pftwo) {}

  void Loop() {
    SDL_Surface *surf = sdlutil::makesurface(256, 256, true);
    SDL_Surface *surfhalf = sdlutil::makesurface(128, 128, true);
    (void)frame;
    (void)surf;
    (void)surfhalf;

    const int num_workers = pftwo->workers.size();
    vector<vector<uint8>> screenshots;
    screenshots.resize(num_workers);
    for (auto &v : screenshots) v.resize(256 * 256 * 4);

    double start = SDL_GetTicks() / 1000.0;

    for (;;) {
      frame++;
      SDL_Event event;
      SDL_PollEvent(&event);
      switch (event.type) {
      case SDL_QUIT:
	return;
      case SDL_KEYDOWN:
	switch (event.key.keysym.sym) {

	case SDLK_ESCAPE:
	  return;
	default:;
	}
	break;
      default:;
      }

      SDL_Delay(1000.0 / 30.0);

      sdlutil::clearsurface(screen, 0x33333333);
      /*
      sdlutil::clearsurface(screen,
			    (frame * 0xDEADBEEF));
      */

      int64 tree_size = ReadWithLock(&pftwo->tree_m, &pftwo->tree->num_nodes);

      font->draw(10, 10, StringPrintf("This is frame %d! %lld tree nodes",
				      frame, tree_size));

      int64 total_steps = 0ll;
      for (int i = 0; i < pftwo->workers.size(); i++) {
	const Worker *w = pftwo->workers[i]->GetWorker();
	int numer = w->numer.load(std::memory_order_relaxed);
	font->draw(256 * 6 + 10, 40 + FONTHEIGHT * i,
		   StringPrintf("[%d] %d/%d %s",
				i,
				numer,
				w->denom.load(std::memory_order_relaxed),
				w->status.load(std::memory_order_relaxed)));
	total_steps += w->nes_frames.load(std::memory_order_relaxed);
      }

      // Update all screenshots.
      vector<vector<string>> texts;
      texts.resize(num_workers);
      for (int i = 0; i < num_workers; i++) {
	Worker *w = pftwo->workers[i]->GetWorker();
	w->Visualize(&screenshots[i]);
	w->VizText(&texts[i]);
      }

      for (int i = 0; i < num_workers; i++) {
	int x = i % 12;
	int y = i / 12;
	BlitARGBHalf(screenshots[i], 256, 256,
		     x * 128, y * 128 + 20, screen);

	for (int t = 0; t < texts[i].size(); t++) {
	  font->draw(x * 128,
		     (y + 1) * 128 + 20 + t * FONTHEIGHT,
		     texts[i][t]);
	}
      }

      // Draw tree. Must hold lock!
      #if 0
      static constexpr int TSTART = 155;
      static constexpr int THEIGHT = HEIGHT * 0.75;
      std::function<void(int x, int y, const Tree::Node *)> DrawTreeRec =
	[this, &DrawTreeRec](int x, int y, const Tree::Node *n) {
	if (x > WIDTH) return;

	const int height_rest = THEIGHT - y;
	
	uint8 rrr = 127, ggg = 127, bbb = 127;
	for (WorkThread *wt : pftwo->workers) {
	  if (wt->IsActiveNode(n)) {
	    rrr = 255;
	    ggg = 0;
	    bbb = 0;
	  }
	}
	
	sdlutil::drawbox(screen, x - 2, TSTART + y - 2, 4, 4, rrr, ggg, bbb);
	const int children = n->children.size();
	const int xc = x + 12;
	for (int i = 0; i < children; i++) {
	  int yc = (height_rest / (float)children) * i;
	  sdlutil::drawline(screen, x, TSTART + y, xc, TSTART + yc, 32, 32, 32);
	}
	int i = 0;
	for (const auto &child : n->children) {
	  int yc = (height_rest / (float)children) * i;
	  DrawTreeRec(xc, yc, child.second);
	  i++;
	}
	
      };

      {
	MutexLock ml(&pftwo->tree_m);
	DrawTreeRec(2, HEIGHT / 2, pftwo->tree->root);
      }
      #endif
      
      const double now = SDL_GetTicks() / 1000.0;
      double sec = now - start;
      font->draw(10, HEIGHT - FONTHEIGHT,
		 StringPrintf("%lld NES frames in %.1f sec = %.2f NES FPS "
			      "%.2f UI FPS",
			      total_steps, sec,
			      total_steps / sec,
			      frame / sec));

     
      SDL_Flip(screen);
    }

  }

  void Run() {
    screen = sdlutil::makescreen(WIDTH, HEIGHT);
    CHECK(screen);

    font = Font::create(screen,
			"font.png",
			FONTCHARS,
			FONTWIDTH, FONTHEIGHT, FONTSTYLES, 1, 3);
    CHECK(font != nullptr) << "Couldn't load font.";


    Loop();
    Printf("UI shutdown.\n");
    WriteWithLock(&should_die_m, &should_die, true);
  }

  ~UIThread() {
    // XXX free screen
    delete font;
  }

 private:
  static constexpr int FONTWIDTH = 9;
  static constexpr int FONTHEIGHT = 16;

  Font *font = nullptr;

  PF2 *pftwo = nullptr;
  SDL_Surface *screen = nullptr;
  int64 frame = 0LL;
};

void PF2::StartThreads() {
  CHECK(workers.empty());
  CHECK(num_workers > 0);
  workers.reserve(num_workers);
  for (int i = 0; i < num_workers; i++) {
    workers.push_back(new WorkThread(this, i));
  }

  ui_thread = new UIThread(this);
  ui_thread->Run();
}

void PF2::DestroyThreads() {
  delete ui_thread;
  ui_thread = nullptr;
  for (WorkThread *wt : workers)
    delete wt;
  workers.clear();
}


// The main loop for SDL.
int main(int argc, char *argv[]) {
  fprintf(stderr, "Init SDL\n");

  /* Initialize SDL and network, if we're using it. */
  CHECK(SDL_Init(SDL_INIT_VIDEO) >= 0);
  fprintf(stderr, "SDL initialized OK.\n");

  {
    PF2 pftwo;
    pftwo.Play();
  }

  SDL_Quit();
  return 0;
}