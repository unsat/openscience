//
// Created by AUTHOR on 3/5/2020.
//

#include "Algo.h"

#include <igen/Context.h>
#include <igen/Domain.h>
#include <igen/Config.h>
#include <igen/runner/ProgramRunnerMt.h>
#include <igen/CoverageStore.h>
#include <igen/c50/CTree.h>

#include <klib/print_stl.h>
#include <klib/vecutils.h>
#include <klib/random.h>

#include <fstream>
#include <csignal>
#include <glog/raw_logging.h>
#include <tsl/robin_set.h>
#include <tsl/robin_map.h>

#include <boost/range/adaptor/reversed.hpp>
#include <boost/circular_buffer.hpp>
#include <boost/container/flat_set.hpp>

namespace igen {

namespace {
static volatile std::sig_atomic_t gSignalStatus = 0;
}

class IterativeAlgorithm : public Object {
public:
    explicit IterativeAlgorithm(PMutContext ctx) : Object(move(ctx)) {}

    ~IterativeAlgorithm() override = default;

#define TREE_DATA(id) auto &locdat = vec_loc_data.at(size_t(id)); auto &tree = locdat.tree; auto &shared_tree = locdat.shared_tree

    void finalize_build_trees() {
        LOG(INFO, "finalize_build_trees");
        map_loc_hash.clear();
        int n_min_cases_in_one_leaf = std::numeric_limits<int>::max();
        int n_rebuilds = 0;
        for (const PLocation &loc : cov()->locs()) {
            if (loc->id() >= int(vec_loc_data.size())) break;
            PLocation &mloc = map_loc_hash[loc->hash()];
            TREE_DATA(loc->id());

            if (mloc == nullptr) {
                shared_tree = nullptr;
                mloc = loc;
                VLOG(20, "Process loc {}: {}", loc->id(), loc->name());
            } else {
                tree = nullptr;
                shared_tree = vec_loc_data.at(size_t(mloc->id())).tree;
                //VLOG(100, "Duplicated loc {}: {} (<=> {}: {})", loc->id(), loc->name(), mloc->id(), mloc->name());
                continue;
            }

            tree = new CTree(ctx()), tree->prepare_data(loc), tree->build_tree();
            VLOG(30, "DECISION TREE (n_min_cases = {}) = \n", tree->n_min_cases()) << (*tree);
            n_min_cases_in_one_leaf = std::min(n_min_cases_in_one_leaf, tree->n_min_cases());
            n_rebuilds++;
        }
        LOG(INFO, "(END finalize_build_trees) unique locations = {}, n_min_cases = {}, n_unvisited = {}",
            n_rebuilds, n_min_cases_in_one_leaf, int(vec_loc_data.size()) - cov()->n_locs());
    }

    static constexpr int REBUILD_THR = 100;

    int get_cur_ignore_thr(int x) {
        static constexpr double L = 110, x0 = 15000, k = -0.000346;
        double res = L / (1.0 + std::exp(-k * (x - x0)));
        return (int) std::max(res, 1.0);
    }

    tsl::robin_set<hash_t> set_conf_hash, set_ran_conf_hash;
    tsl::robin_map<hash_t, PLocation> map_loc_hash;

    struct LocData {
        PMutCTree tree;
        PCTree shared_tree;
        boost::circular_buffer_space_optimized<int> rebuild_iter{{REBUILD_THR, 10}};

        bool ignored_ = false;

        bool ignored() const { return ignored_; }
    };

    vec<LocData> vec_loc_data;
    bool iter_try_nodes = true;

    bool alg_test_1_iteration([[maybe_unused]] int iter) {
        vec_loc_data.resize(size_t(cov()->n_locs()));

        vec<PCNode> leaves;
        int n_min_cases_in_one_leaf = std::numeric_limits<int>::max(), n_uniq_locs = 0;
        map_loc_hash.clear();
        for (const PLocation &loc : cov()->locs()) {
            PLocation &mloc = map_loc_hash[loc->hash()];
            TREE_DATA(loc->id());

            if (mloc == nullptr) {
                shared_tree = nullptr;
                mloc = loc;
                n_uniq_locs++;
                VLOG(20, "Process loc {}: {}", loc->id(), loc->name());
            } else {
                tree = nullptr;
                shared_tree = vec_loc_data.at(size_t(mloc->id())).tree;
                VLOG(100, "Duplicated loc {}: {} (<=> {}: {})", loc->id(), loc->name(), mloc->id(), mloc->name());
                continue;
            }

            if (tree == nullptr) {
                tree = new CTree(ctx()), tree->prepare_data(loc), tree->build_tree();
                VLOG(30, "NEW DECISION TREE (n_min_cases = {}) = \n", tree->n_min_cases()) << (*tree);

                n_min_cases_in_one_leaf = std::min(n_min_cases_in_one_leaf, tree->n_min_cases());
                if (leaves.empty())
                    tree->gather_nodes(leaves, 0, 16);
            }
        }
        if (leaves.empty()) {
            for (const PLocation &loc : cov()->locs()) {
                TREE_DATA(loc->id());
                (void) shared_tree;
                if (!locdat.ignored() && tree != nullptr) {
                    n_min_cases_in_one_leaf = std::min(n_min_cases_in_one_leaf, tree->n_min_cases());
                    tree->gather_nodes(leaves, 0, cov()->n_configs() - 1);
                }
            }
        }
        // ============================================================================================================

        std::sort(leaves.begin(), leaves.end(), [](const PCNode &a, const PCNode &b) {
            if (a->n_min_cases() != b->n_min_cases())
                return a->n_min_cases() < b->n_min_cases();
            else
                return a->depth() > b->depth();
        });

        vec<PMutConfig> cex;
        int max_min_cases = 0, skipped = 0;
        const auto gen_for = [this, &max_min_cases, &skipped, &cex](
                const PCNode &node, int lim = std::numeric_limits<int>::max()) {
            int skipped_me = 0;
            max_min_cases = std::max(max_min_cases, node->n_min_cases());
            for (auto &c : node->gen_one_convering_configs(lim)) {
                if (set_conf_hash.insert(c->hash()).second) { cex.emplace_back(move(c)); }
                else { skipped++, skipped_me++; }
            }
            return skipped_me;
        };
        if (iter_try_nodes) {
            for (const PCNode &node : leaves) {
                if (node->n_min_cases() > 0 && cex.size() > 10) break;
                if (cex.size() > 20) break;
                gen_for(node);
            }
        }
        LOG(INFO, "Gen  {:>2} cex, skipped {}, max_min_cases = {}", cex.size(), skipped, max_min_cases);

        if (iter_try_nodes && terminate_counter >= 4) {
            int rand_skip = 0;
            while (cex.size() < 40) {
                const PCNode &node = *Rand.get(leaves);
                if (gen_for(node) > 0 && ++rand_skip == 10) break;
            }
            LOG(INFO, "Rand {:>2} cex, skipped {}, max_min_cases = {}", cex.size(), skipped, max_min_cases);
            for (const PCNode &node : boost::adaptors::reverse(leaves)) {
                if (cex.size() > 50) break;
                gen_for(node);
            }
            LOG(INFO, "Rev  {:>2} cex, skipped {}, max_min_cases = {}", cex.size(), skipped, max_min_cases);
        }

        // === Try rand
        int cex_rand_tried = 0;
        while (cex.empty()) {
            LOG(WARNING, "Can't gen cex, try random configs");
            const auto &vp = dom()->gen_one_convering_configs();
            for (auto &c : vp) {
                if (set_conf_hash.insert(c->hash()).second) cex.emplace_back(move(c));
            }
            if (++cex_rand_tried == 30) break;
        }

        LOG_IF(INFO, cex_rand_tried, "Generated total {} cex", cex.size());
//        LOG_BLOCK(INFO, {
//            log << "CEX:\n";
//            for (const auto &c : cex) log << *c << '\n';
//        });
        if (cex.empty()) return false;

        // ============================================================================================================

        run_configs(cex);

        int n_new_locs = cov()->n_locs() - int(vec_loc_data.size());
        int n_rebuilds = 0, n_rebuilds_uniq = 0;
        int ig_th = get_cur_ignore_thr(iter);
        for (const PMutConfig &c : cex) {
            const vec<int> cov_ids = c->cov_loc_ids();
            auto it = cov_ids.begin();
            for (const PLocation &loc : cov()->locs()) {
                if (loc->id() >= int(vec_loc_data.size())) break;
                TREE_DATA(loc->id());
                auto &re_iter = locdat.rebuild_iter;
                if (tree == nullptr && shared_tree == nullptr) continue;
                if (locdat.ignored()) continue;

                bool new_truth = false;
                if (it != cov_ids.end() && *it == loc->id()) new_truth = true, ++it;
                bool tree_eval = (tree != nullptr ?
                                  tree->test_add_config(c, new_truth).first : shared_tree->test_config(c).first);

                bool tree_need_rebuild = (tree_eval != new_truth);
                if (tree_need_rebuild) {
                    re_iter.push_back(iter);
                    while (re_iter.size() && re_iter.front() < iter - REBUILD_THR) re_iter.pop_front();
                    if ((int) re_iter.size() > ig_th) {
                        LOG(WARNING, "Ignored loc ({}) {}.   re_iter={}, ig_th={}",
                            loc->id(), loc->name(), re_iter.size(), ig_th);
                        locdat.ignored_ = true;
                        continue;
                    }

                    n_rebuilds++;
                    if (tree != nullptr) n_rebuilds_uniq++;
                    tree = nullptr, shared_tree = nullptr;
                    VLOG(20, "Need rebuild loc {}", loc->name());
                    continue;
                } else {
                    while (re_iter.size() && re_iter.front() < iter - REBUILD_THR) re_iter.pop_front();
                }
            }
        }

        const auto &prunner = ctx()->runner();
        LOG(INFO, "    {:^10}{:^6}{:^10}{:^10}{:^8}{:^7} {:^8}{:^7}{:^6} {:^8}{:^9}{:^10}",
            "rebuilds", "uniq", "new_locs", "min_cases", "leaves", "ig_th",
            "configs", "locs", "uniq",
            "runs", "all_locs", "cache_hit");
        LOG(INFO, "    {:^10}{:^6}{:^10}{:^10}{:^8}{:^7} {:^8}{:^7}{:^6} {:^8}{:^9}{:^10}",
            n_rebuilds, n_rebuilds_uniq, n_new_locs, n_min_cases_in_one_leaf, leaves.size(), ig_th,
            cov()->n_configs(), cov()->n_locs(), n_uniq_locs,
            prunner->n_runs(), prunner->n_locs(), prunner->n_cache_hit());
        bool need_term = n_rebuilds == 0 && n_new_locs == 0 && n_min_cases_in_one_leaf > 0;
        LOG_IF(WARNING, need_term, "need_term = TRUE, terminate_counter = {:>2}", terminate_counter + 1);
        if (need_term) {
            if (++terminate_counter == max_terminate_counter) return false;
        } else {
            terminate_counter = 0;
        }
        return true;
    }

    int terminate_counter = 0;
    int max_terminate_counter = 0;

    void run_alg_test_1() {
        max_terminate_counter = ctx()->get_option_as<int>("term-cnt");

        vec<PMutConfig> init_configs;
        if (ctx()->has_option("full")) {
            init_configs = dom()->gen_all_configs();
            iter_try_nodes = false;
        } else {
            init_configs = dom()->gen_one_convering_configs();
            int n_one_covering = int(init_configs.size());
            auto seed_configs = get_seed_configs();
            int n_seed_configs = int(seed_configs.size());
            vec_move_append(init_configs, seed_configs);
            VLOG(10, "Run initial configs (n_one_covering = {}, n_seed_configs = {})", n_one_covering, n_seed_configs);
        }

        LOG(INFO, "Running {} init configs", init_configs.size());
        for (const auto &c : init_configs) set_conf_hash.insert(c->hash());
        run_configs(init_configs);
        LOG(INFO, "Done run {} init configs", init_configs.size());

        int N_ROUNDS = ctx()->get_option_as<int>("rounds");
        for (int iter = 1; iter <= N_ROUNDS; ++iter) {
            LOG(INFO, "{:=^80}", fmt::format("  Iteration {}  ", iter));
            if (!alg_test_1_iteration(iter)) {
                LOG(WARNING, "Early break at iteration {}", iter);
                break;
            }
            if (gSignalStatus == SIGINT) {
                LOG(WARNING, "Requested break at iteration {}", iter);
                ctx()->runner()->flush_cachedb();
                break;
            }
            if (iter % 100 == 0) {
                finalize_build_trees();
                finish_alg1(false);
            }
        }

        finalize_build_trees();

        finish_alg1();
    }

    void finish_alg1(bool expensive_simplify = true) {
        bool out_to_file = ctx()->has_option("output");

        struct LineEnt {
            vec<PLocation> locs;
            str expr;
            int rebuild_iter;
            bool ignored;
        };
        vec<LineEnt> ents;
        if (out_to_file) ents.resize(vec_loc_data.size());

        LOG(INFO, "{:=^80}", "  FINAL RESULT  ");
        map_loc_hash.clear();
        auto expr_strat = CTree::FreeMix;
        if (ctx()->has_option("disj-conj")) expr_strat = CTree::DisjOfConj;
        int simpl_cnt = 0;
        for (const PLocation &loc : cov()->locs()) {
            if (loc->id() >= int(vec_loc_data.size())) break;
            std::stringstream log;
            TREE_DATA(loc->id());
            PLocation &mloc = map_loc_hash[loc->hash()];
            fmt::print(log, "{:>5}: {:<16}  ==>  ", loc->id(), loc->name());
            if (mloc == nullptr) {
                bool do_simpl = expensive_simplify && !locdat.ignored();
                LOG_IF(INFO, do_simpl, "Simplifying expr: {:>3} ({}) {}", ++simpl_cnt, loc->id(), loc->name());
                CHECK_NE(tree, nullptr);
                mloc = loc;
                z3::expr e = tree->build_zexpr(expr_strat);
                e = e.simplify();
                if (do_simpl) e = ctx()->zctx_solver_simplify(e);

                if (out_to_file) {
                    auto &et = ents.at(size_t(loc->id()));
                    et.locs.push_back(loc);
                    et.expr = e.to_string();
                    et.rebuild_iter = (int) locdat.rebuild_iter.size();
                    et.ignored = locdat.ignored();
                    log << et.expr;
                } else {
                    log << e;
                }
            } else {
                CHECK_NE(shared_tree, nullptr);
                fmt::print(log, "{}: {}", mloc->id(), mloc->name());
                if (out_to_file) ents.at(size_t(mloc->id())).locs.push_back(loc);
            }
            if (out_to_file) {
                GVLOG(1) << log.rdbuf();
            } else {
                GLOG(INFO) << log.rdbuf();
            }
        }
        LOG(INFO, "configs = {}, locs = {}, uniq_locs = {}",
            cov()->n_configs(), cov()->n_locs(), map_loc_hash.size());
        const auto &prunner = ctx()->runner();
        LOG(INFO, "Runner stat: n_runs = {}, n_total_locs = {}, cache_hit = {}",
            prunner->n_runs(), prunner->n_locs(), prunner->n_cache_hit());

        if (out_to_file) {
            std::ofstream outstream;
            str fout = ctx()->get_option_as<str>("output");
            outstream.open(fout);
            CHECKF(!outstream.fail(), "Can't open output file {}", fout);

            fmt::print(outstream, "# seed = {}\n", ctx()->get_option_as<uint64_t>("seed"));
            fmt::print(outstream,
                       "# configs = {}, locs = {}, uniq_locs = {}\n# n_runs = {}, n_total_locs = {}\n======\n",
                       cov()->n_configs(), cov()->n_locs(), map_loc_hash.size(),
                       ctx()->runner()->n_runs(), ctx()->runner()->n_locs());
            for (const auto &e : ents) {
                if (e.locs.empty()) continue;
                fmt::print(outstream, "# rebuild_iter = {:>5} / {}{}\n", e.rebuild_iter, REBUILD_THR,
                           e.ignored ? "    (IGNORED)" : "");
                for (const auto &loc : e.locs)
                    outstream << loc->name() << ", ";
                outstream << "\n-\n" << e.expr << "\n======\n";
            }
        }
    }

    void run_configs(const vec<PMutConfig> &configs) {
        auto v_loc_names = ctx()->runner()->run(configs);
        CHECK_EQ(configs.size(), v_loc_names.size());
        for (int i = 0; i < sz(configs); ++i) {
            const auto &c = configs[i];
            const auto &loc_names = v_loc_names[i];
            cov_mut()->register_cov(c, loc_names);
            bool insert_new = set_ran_conf_hash.insert(c->hash()).second;
            CHECK(insert_new);
            if (dom()->n_vars() <= 16 && loc_names.size() <= 16) {
                VLOG(50, "{}  ==>  ", *c) << loc_names;
            } else {
                VLOG(50, "Config {}  ==>  {} locs", c->id(), loc_names.size());
            }
        }
    }

    void run_alg() {
        std::signal(SIGINT, [](int signal) {
            if (gSignalStatus) {
                RAW_LOG(ERROR, "Force terminate");
                exit(1);
            }
            gSignalStatus = signal;
        });
        switch (ctx()->get_option_as<int>("alg-version")) {
            case 1:
                return run_alg_test_1();
            default:
                CHECK(0);
        }
    }

    vec<PMutConfig> get_seed_configs() const {
        vec<PMutConfig> ret;
        if (ctx()->has_option("seed-configs")) {
            vec<str> vs = ctx()->get_option_as<vec<str>>("seed-configs");
            for (const auto &s : vs)
                ret.emplace_back(new Config(ctx_mut(), s));
        }
        return ret;
    }

private:
};

int run_interative_algorithm(const map<str, boost::any> &opts) {
    PMutContext ctx = new Context();
    ctx->set_options(opts);
    ctx->init();
    ctx->init_runner();
    {
        ptr<IterativeAlgorithm> ite_alg = new IterativeAlgorithm(ctx);
        ite_alg->run_alg();
    }
    ctx->cleanup();
    return 0;
}


}