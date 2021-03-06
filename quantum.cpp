#include <iostream>
#include <csignal>
#include <cstdlib>
#include <chrono>
#include <unistd.h> // isatty()

#ifdef BENCH
  #define GENETIC_OPENMP_REPRODUCIBLE
#endif

#include "QGA_full.hpp"
#include "Colours.hpp"
#include "BriefPrinter.hpp"
#include "CircuitPrinter.hpp"
#include "TeXPrinter.hpp"
#include "signal.hpp"
#include "popl.hpp"

#ifdef FOURIER
  #include "QGA_Problem/Fourier.hpp"
  #ifdef USE_QPP
    #error FFT implementation is currently broken in Eigen3, used by Quantum++.
  #endif
#elif defined(SEARCH)
  #include "QGA_Problem/Search.hpp"
#elif defined(QPCA)
  #include "QGA_Problem/QPCA.hpp"
#else
  #include "QGA_Problem/Simple.hpp"
#endif

namespace Config {

  // Circuit width (pre-set)
  unsigned nBit = 5;

  // strength parameter of NSGA selection
  double selectBias = 1.0;

  // Internal population size
  size_t popSize = 1000;

  // Archive (external population) size
  size_t arSize = 100;

  // Generation limit
  unsigned long maxGen = 0;

  // Expected curcuit depth in 0th generation
  double expLengthIni = 30;

  // Expected number of single-point mutations (including crossover)
  double expMutationCount = 2.0;

  // Expected number of successive gates to be inserted / deleted / replaced
  // NB: 1 is always added to slice length
  double expSliceLength = 2.0;

  // How much each bit is likely to be a control bit at gate creation
  const double pControl = 0.5;

  // Size of random subset of candidates to list on demand at interrupt
  const size_t nIntList = 20;

  // Standard deviation of mutation in gate angles
  const double dAlpha = 0.2;

  // Maximum length of an output line when formatting circuits
  const size_t circLineLength = 220;

} // namespace Config


// Initialization of global variables
namespace Signal {
  volatile sig_atomic_t state = RUNNING;
  std::chrono::duration<double> timeOut{};
}


// Candidate defined in PROBLEM_HPP
using Population = gen::NSGAPopulation<Candidate>;
using GenCandidate = gen::Candidate<Candidate>;
using CandidateFactory = QGA::CandidateFactory<Candidate>;
using GenOpCounter = QGA::GenOpCounter<CandidateFactory>;

template<>
constexpr decltype(CandidateFactory::ops) CandidateFactory::ops;

/* Forward declarations */
void int_handler(int);
int int_response(Population&, unsigned long);
void dumpResults(Population&, GenOpCounter&,
    std::chrono::time_point<std::chrono::steady_clock>, unsigned long, unsigned long);
/* End forward declarations */

BriefPrinter<Candidate> brief(const Candidate& ref) {
  return {ref};
}


/************
 *** Main ***
 ************/

int main(int argc, char* argv[]) {
  /* Initialize output */
  if(isatty(1)) {
    Colours::use = true;
    std::signal(SIGINT, int_handler);
  }
  std::cout << std::fixed << std::setprecision(4);

  /* Parse command line */
  {
    popl::OptionParser op("Usage");
    op.add<popl::Value<unsigned>>("b", "bits", "number of qubits",
        Config::nBit, &Config::nBit);
    op.add<popl::Value<size_t>>("p", "pop", "population size",
        Config::popSize, &Config::popSize);
    op.add<popl::Value<size_t>>("a", "archive", "archive size",
        Config::arSize, &Config::arSize);
    op.add<popl::Value<unsigned long>>("g", "gen", "generation limit",
        Config::maxGen, &Config::maxGen);
    op.add<popl::Value<double>>("i", "ini", "expected length of gen=0 circuits",
        Config::expLengthIni, &Config::expLengthIni);
    op.add<popl::Value<double>>("s", "press", "selection pressure",
        Config::selectBias, &Config::selectBias);
    op.add<popl::Value<double>>("m", "emc", "expected mutation count",
        Config::expMutationCount, &Config::expMutationCount);
    op.add<popl::Value<double>>("l", "slice", "expected slice length (minus 1)",
        Config::expSliceLength, &Config::expSliceLength);

    bool help;
    op.add<popl::Switch>("h", "help", "show this help message", &help);

    op.parse(argc, argv);
    bool bad = (op.non_option_args().size() > 0
        || op.unknown_options().size() > 0);
    if(help || bad) {
      std::cout << op;
      return bad ? 1 : 0;
    }
  }

#ifdef BENCH
  /* Set a fixed number of threads */
  omp_set_num_threads(4);
  omp_set_dynamic(false);

  /* Initialize the RNG's */
  {
    std::mt19937 master{};
    #pragma omp parallel for ordered
    for(int j = 0; j < omp_get_num_threads(); j++)
      #pragma omp ordered
      gen::rng.seed(master());
  }
#endif

  /* Initialize state variables */
  std::chrono::time_point<std::chrono::steady_clock>
    start{std::chrono::steady_clock::now()};
  Population pop{Config::popSize,
    [] { return CandidateFactory::genInit().setGen(0); }};
  GenOpCounter trk{};
  unsigned long total_count = 0;
  unsigned long gen;

  /* Main loop */
  for(gen = 0; Config::maxGen == 0 || gen <= Config::maxGen; gen++) {

    /* Find the nondominated subset */
    Population pop2 = pop.front();

    /* Randomize and drop very similar fitnesses (disregarding gate counts) */
    pop2.prune([](const GenCandidate& a, const GenCandidate& b) -> bool {
        return dist(a.fitness(), b.fitness()) < 0.1;
      }, 0, true);

    /* Rank-trim the rest down to arSize */
    pop2.rankTrim(Config::arSize);

    /* Unconditionally add the best candidate so far (in case it got pruned) */
    pop2.add(pop.best());

    /* Top up to popSize candidates in parallel */
    CandidateFactory cf{pop};
    pop.precompute();
    size_t topup_count = Config::popSize - pop2.size();
    pop2.add(topup_count, [&] { return cf.getNew().setGen(gen); });
    total_count += topup_count;

    /* We don't need the original population anymore */
    pop = std::move(pop2);

    /* Leave only one representative of each fitness and drop dominated
     * versions of the same circuit */
    pop.prune([](const GenCandidate& a, const GenCandidate& b) -> int {
        if(a.fitness() == b.fitness())
          return 1;//a.getGen() < b.getGen() ? 1 : -1;
        else if(sameCirc(a, b))
          return b << a ? -1 : 1;
        else
          return 0;
      }, 0, false);

    /* Take a record which GenOps were successful in making good candidates */
    auto nondom = pop.front();
    for(auto& c : nondom)
      if(c.getGen() == gen)
        trk.hit(c.getOrigin());

    /* Summarize */
    {
      // Prepare circuit in advance to not delay the printing operation later
      auto circuit = pop.best().circuit<CircuitPrinter>();
      std::cout
        << Colours::bold("Gen ", gen, ": ")
        << Colours::yellow(pop.size()) << " unique fitnesses, "
        << "lowest error " << brief(pop.best()) << ", "
        << Colours::yellow(nondom.size()) << " nondominated\n"
        << circuit << std::endl;
    }

    /* Interrupted? */
    while(Signal::state == Signal::INTERRUPTED)
      switch(int_response(pop, gen)) {
        case Signal::DUMP:
          dumpResults(pop, trk, start, total_count, gen);
          break;
        case Signal::RESTART:
          pop = Population{Config::popSize,
            [&] { return CandidateFactory::genInit().setGen(0); }};
          trk.reset();
          total_count = 0;
          start = std::chrono::steady_clock::now();
          Signal::timeOut = std::chrono::duration<double>(0);
          gen = 0;
          break;
      }
    if(Signal::state == Signal::STOPPING)
      break;
  }

  dumpResults(pop, trk, start, total_count, gen);
}


void dumpResults(Population& pop, GenOpCounter& trk,
    std::chrono::time_point<std::chrono::steady_clock> start,
    unsigned long total_count, unsigned long gen) {

  /* List results */
  auto nondom = pop.front();
  nondom.sort();
  std::cout << '\n'
    << Colours::yellow(nondom.size()) << " nondominated candidates:\n";
  for(auto& c : nondom.reverse()) {
    std::cout << brief(c) << ' ' << c;
    if(c.fitness().head() < 0.01)
      std::cout << ": " << c.full() << c.circuit<CircuitPrinter>();
    else
      std::cout << '\n';
  }

  /* Dump the operator statistics */
  std::cout << "\nGenetic operator success rates:\n" << trk;

  /* Timing information */
  std::chrono::time_point<std::chrono::steady_clock>
    now{std::chrono::steady_clock::now()};
  std::chrono::duration<double> dur = now - start - Signal::timeOut;
  std::cout
    << "\nRun took " << dur.count() << " s, "
    << Colours::blue(total_count) << " candidates tested in "
    << Colours::blue(gen) << " generations "
    << '(' << Colours::blue(dur.count()/gen, " s/gen") << " avg)\n";
}


/* Helper functions for interrupt handler */

Candidate input() {
  std::cout << "Enter a candidate:\n";
  std::string s{};
  std::getline(std::cin, s);
  return Candidate::read(s);
}

void listRandom(Population& pop) {
  auto sel = pop.randomSelect(Config::nIntList);
  sel.sort();
  for(auto& c : sel.reverse())
    std::cout << brief(c) << ' ' << c << '\n';
}

void listFilter(Population& pop) {
  std::cout << "Enter space-separated maximum elements of fitness "
    << "(non-number for no filter on a field):\n";
  std::string line{};
  std::getline(std::cin, line);
  std::istringstream is{line};
  GenCandidate::Traits::FitnessType maxFitness{};
  is >> maxFitness;

  auto nondom = pop.front();
  nondom.prune([&](const GenCandidate& c) -> bool {
      return !(c.fitness() << maxFitness);
    });
  nondom.sort();
  std::cout << '\n'
    << Colours::yellow(nondom.size()) << " nondominated candidates:\n";
  for(auto& c : nondom.reverse())
    std::cout << brief(c) << ' ' << c << '\n';
}

void evaluate() {
  Candidate c{input()};
  std::cout << "\nParsed: " << brief(c) << ' ' << c << '\n'
    << c.full() << '\n';
}

void inject(Population& pop, unsigned long gen) {
  Candidate c{input()};
  c.setGen(gen);
  pop.add(c);
  std::cout << "\nParsed: " << brief(c) << ' ' << c << '\n';
}

template<class Printer>
void prettyprint() {
  Candidate c{input()};
  std::cout << c.circuit<Printer>() << '\n';
}

/* Interrupt handler (Ctrl-C) */

void int_handler(int) {
  if(Signal::state != Signal::RUNNING)
    // getting here means we got stuck while processing another signal
    // (e.g., popSize too large or a deadlock)
    std::_Exit(1);
  Signal::state = Signal::INTERRUPTED;
}

/* Interrupt diagnosis */

int int_response(Population& pop, unsigned long gen) {
  std::chrono::time_point<std::chrono::steady_clock> pre, post;
  pre = std::chrono::steady_clock::now();
  std::cerr << "\nComputation stopped. Choose action:\n"
    << Colours::blue("a: ") << "abort,\n"
    << Colours::blue("c: ") << "continue,\n"
    << Colours::blue("d: ") << "diagnose / list current results,\n"
    << Colours::blue("e: ") << "evaluate a candidate in full,\n"
    << Colours::blue("f: ") << "filter the front on fitness,\n"
    << Colours::blue("i: ") << "inject a candidate,\n"
    << Colours::blue("l: ") << "list " << Config::nIntList
        << " random candidates,\n"
    << Colours::blue("p: ") << "pretty-print a candidate as a circuit,\n"
    << Colours::blue("r: ") << "restart,\n"
    << Colours::blue("t: ") << "format a candidate as a LuaLaTeX Q-circuit,\n"
    << Colours::blue("q: ") << "quit after this generation.\n";
  int ret = -1;
  do {
    char c;
    std::cerr << "\nYour choice: ";
    std::cin >> c;
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    if(std::cin.eof())
      c = 'a';
    switch(c) {
      case 'a':
        std::exit(1);
      case 'c':
        Signal::state = Signal::RUNNING;
        ret = Signal::CONTINUE;
        break;
      case 'd':
        ret = Signal::DUMP;
        break;
      case 'e':
        evaluate();
        return int_response(pop, gen); // tail recursion
      case 'f':
        listFilter(pop);
        return int_response(pop, gen);
      case 'i':
        inject(pop, gen);
        return int_response(pop, gen);
      case 'l':
        listRandom(pop);
        return int_response(pop, gen);
      case 'p':
        prettyprint<CircuitPrinter>();
        return int_response(pop, gen);
      case 'r':
        Signal::state = Signal::RUNNING;
        ret = Signal::RESTART;
        break;
      case 't':
        prettyprint<TeXPrinter>();
        return int_response(pop, gen);
      case 'q':
        Signal::state = Signal::STOPPING;
        ret = Signal::STOP;
        break;
    }
  } while(ret < 0);
  post = std::chrono::steady_clock::now();
  Signal::timeOut += post - pre;
  return ret;
}
