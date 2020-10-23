#include "bdd_storage.h"

namespace LPMP {

    bdd_storage::bdd_storage(bdd_preprocessor& bdd_pre)
    {
        for(size_t bdd_nr=0; bdd_nr<bdd_pre.get_bdd_collection().nr_bdds(); ++bdd_nr)
            add_bdd(bdd_pre.get_bdd_collection()[bdd_nr]);
    }

    bdd_storage::bdd_storage()
    {}

    void bdd_storage::add_bdd(BDD::bdd_collection_entry bdd)
    {
        const auto vars = bdd.variables();
        std::unordered_map<size_t,size_t> rebase_to_iota;
        for(size_t i=0; i<vars.size(); ++i)
            rebase_to_iota.insert({vars[i], i});
        bdd.rebase(rebase_to_iota);

        auto get_node = [&](const size_t i) {
            const size_t j = bdd.nr_nodes() - 3 - i;
            return bdd[j]; 
        };
        auto get_next_node = [&](BDD::bdd_collection_node node) { return node.next_postorder(); };
        auto get_variable = [&](BDD::bdd_collection_node node) { return node.variable(); };

        add_bdd_impl<BDD::bdd_collection_node>(
                bdd.nr_nodes() - 2, // do not count in top- and botsink
                get_node,
                get_variable,
                get_next_node,
                vars.begin(), vars.end()
                );

        bdd.rebase(vars.begin(), vars.end());
    }

    inline void bdd_storage::check_node_valid(const bdd_node bdd) const
    {
        assert(bdd.low == bdd_node::terminal_0 || bdd.low == bdd_node::terminal_1 || bdd.low < bdd_nodes_.size());
        assert(bdd.high == bdd_node::terminal_0 || bdd.high == bdd_node::terminal_1 || bdd.high < bdd_nodes_.size());
        assert(bdd.variable < nr_variables());
        if(bdd.low != bdd_node::terminal_0 && bdd.low != bdd_node::terminal_1) {
            assert(bdd.variable < bdd_nodes_[bdd.low].variable);
        }
        if(bdd.high != bdd_node::terminal_0 && bdd.high != bdd_node::terminal_1) {
            assert(bdd.variable < bdd_nodes_[bdd.high].variable);
        }
        //assert(bdd.high != bdd.low); this can be so in ou formulation, but not in ordinary BDDs
    }


    std::size_t bdd_storage::first_bdd_node(const std::size_t bdd_nr) const
    {
        assert(bdd_nr < nr_bdds());
        return bdd_nodes_[bdd_delimiters_[bdd_nr]].variable;
    }

    std::size_t bdd_storage::last_bdd_node(const std::size_t bdd_nr) const
    {
        assert(bdd_nr < nr_bdds());
        std::size_t max_node = 0;
        for(std::size_t i=bdd_delimiters_[bdd_nr]; i<bdd_delimiters_[bdd_nr+1]; ++i)
            max_node = std::max(max_node, bdd_nodes_[i].variable);
        return max_node;
    }

    std::vector<std::array<size_t,2>> bdd_storage::dependency_graph() const
    {
        std::unordered_set<std::array<size_t,2>> edges;
        std::unordered_set<size_t> cur_vars;
        std::vector<size_t> cur_vars_sorted;
        for(size_t bdd_nr=0; bdd_nr<nr_bdds(); ++bdd_nr)
        {
            cur_vars.clear();
            for(size_t i=bdd_delimiters_[bdd_nr]; i<bdd_delimiters_[bdd_nr+1]; ++i)
                cur_vars.insert(bdd_nodes_[i].variable);
            cur_vars_sorted.clear();
            for(const size_t v : cur_vars)
                cur_vars_sorted.push_back(v);
            std::sort(cur_vars_sorted.begin(), cur_vars_sorted.end());
            for(size_t i=0; i+1<cur_vars_sorted.size(); ++i)
                edges.insert({cur_vars_sorted[i], cur_vars_sorted[i+1]});
        }

        return std::vector<std::array<size_t,2>>(edges.begin(), edges.end());
    }

    ///////////////////////////
    // for BDD decomposition //
    ///////////////////////////

    bdd_storage::intervals bdd_storage::compute_intervals(const size_t nr_intervals)
    {
        // first approach: Just partition variables equidistantly.
        // TODO: Take into account number of BDD nodes in each interval
        
        assert(nr_intervals > 1);
        std::vector<size_t> interval_boundaries;
        interval_boundaries.reserve(nr_intervals+1);
        for(size_t interval=0; interval<nr_intervals; ++interval)
            interval_boundaries.push_back(std::round(double(interval*this->nr_variables())/double(nr_intervals)));

        interval_boundaries.push_back(this->nr_variables()); 
        assert(interval_boundaries.size() == nr_intervals+1);

        std::vector<size_t> variable_interval;
        variable_interval.reserve(this->nr_variables());
        for(size_t interval = 0; interval+1<interval_boundaries.size(); ++interval)
            for(size_t var=interval_boundaries[interval]; var<interval_boundaries[interval+1]; ++var)
                variable_interval.push_back(interval);

        return intervals{variable_interval, interval_boundaries};
    }

    size_t bdd_storage::intervals::interval(const size_t variable) const
    {
        assert(variable < this->variable_interval.size());
        assert(interval_boundaries.size() > 2 || interval_boundaries.size() == 0);
        if(interval_boundaries.size() == 0)
            return 0;
        return variable_interval[variable];
    }

    size_t bdd_storage::intervals::nr_intervals() const
    {
        assert(interval_boundaries.size() > 2 || interval_boundaries.size() == 0);
        if(interval_boundaries.size() == 0)
            return 1;
        return interval_boundaries.size()-1;
    }

    // take bdd_nodes_ and return two_dim_variable_array<bdd_node> bdd_nodes_split_, two_dim_variable_array<size_t> bdd_delimiters_split_
    // TODO: do not use nr_bdd_nodes_per_interval in second part, i.e. filling in bdd nodes. bdd_nodes_.size() is enough
    std::tuple<std::vector<bdd_storage>, std::unordered_set<bdd_storage::duplicate_variable, bdd_storage::duplicate_variable_hash>> bdd_storage::split_bdd_nodes(const size_t nr_intervals)
    {
        assert(nr_intervals > 1);
        intervals intn = compute_intervals(nr_intervals);
        assert(nr_intervals == intn.nr_intervals());

        std::vector<size_t> nr_bdd_nodes_per_interval(nr_intervals, 0);
        std::vector<size_t> nr_bdds_per_interval(nr_intervals, 1);
        std::unordered_set<size_t> active_intervals;

        for(size_t bdd_counter=0; bdd_counter<bdd_delimiters_.size()-1; ++bdd_counter)
        {
            const size_t last_bdd_interval = [&]() {
                size_t last_bdd_interval = 0;
                for(auto bdd_node_counter=bdd_delimiters_[bdd_counter]; bdd_node_counter<bdd_delimiters_[bdd_counter+1]; ++bdd_node_counter)
                {
                    const auto& bdd = bdd_nodes_[bdd_node_counter];
                    last_bdd_interval = std::max(intn.interval(bdd.variable), last_bdd_interval);
                }
                return last_bdd_interval;
            }();
            
            // there are the following cases:
            // (i) All bdd nodes are non-terminal and in the same interval
            // (ii) all bdd nodes are non-terminal but in different intervals
            // (iii) one arc is bottom and the other one is in the same interval
            // (iv) one arc is bottom and the other one is in the next interval
            // (v) At least one arc is top -> bdd node is in last interval
            
            // count in which intervals bdd has nodes
            active_intervals.clear();
            for(auto bdd_node_counter=bdd_delimiters_[bdd_counter]; bdd_node_counter<bdd_delimiters_[bdd_counter+1]; ++bdd_node_counter)
            {
                const auto& bdd = bdd_nodes_[bdd_node_counter];
                active_intervals.insert(intn.interval(bdd.variable));
            }
            for(const size_t i : active_intervals)
                ++nr_bdds_per_interval[i];

            // count number of bdd nodes per interval
            for(auto bdd_node_counter=bdd_delimiters_[bdd_counter]; bdd_node_counter<bdd_delimiters_[bdd_counter+1]; ++bdd_node_counter)
            {
                const auto& bdd = bdd_nodes_[bdd_node_counter];
                // first check if node is split node. If not, increase node count in correct interval. If split node, increment node count in both intervals straddled.
                // case (i)
                if(!bdd.low_is_terminal() && !bdd.high_is_terminal())
                {
                    const bdd_node& low = bdd_nodes_[bdd.low];
                    const bdd_node& high = bdd_nodes_[bdd.high];
                    assert(low.variable == high.variable);
                    if(intn.interval(bdd.variable) == intn.interval(low.variable)) // case (i)
                    {
                        ++nr_bdd_nodes_per_interval[intn.interval(bdd.variable)]; 
                    }
                    else // case (ii)
                    {
                        ++nr_bdd_nodes_per_interval[intn.interval(bdd.variable)];
                        ++nr_bdd_nodes_per_interval[intn.interval(bdd_nodes_[bdd.low].variable)]; // bdd nodes pointed  to by low and high will be counted in next interval again when visiting those 
                    }
                }
                else if(bdd.low_is_terminal() && bdd.high_is_terminal()) // case (v)
                {
                    assert(bdd.low == bdd_node::terminal_1 || bdd.high == bdd_node::terminal_1);
                    ++nr_bdd_nodes_per_interval[intn.interval(bdd.variable)]; 
                }
                else if(bdd.low == bdd_node::terminal_0 || bdd.high == bdd_node::terminal_0)
                {
                    if(bdd.low == bdd_node::terminal_0)
                    {
                        const size_t high_var = bdd_nodes_[bdd.high].variable;
                        if(intn.interval(bdd.variable) == intn.interval(high_var)) // case (iii)
                        {
                            ++nr_bdd_nodes_per_interval[intn.interval(bdd.variable)];
                        }
                        else // case (iv)
                        {
                            // TODO: not necessarily += 2, possibly += 1 if node is shared!
                            ++nr_bdd_nodes_per_interval[intn.interval(bdd.variable)];
                            ++nr_bdd_nodes_per_interval[intn.interval(high_var)];
                        }
                    }
                    else
                    {
                        assert(bdd.high == bdd_node::terminal_0);
                        const size_t low_var = bdd_nodes_[bdd.low].variable;
                        if(intn.interval(bdd.variable) == intn.interval(low_var)) // case (iii)
                        {
                            ++nr_bdd_nodes_per_interval[intn.interval(bdd.variable)];
                        }
                        else // case (iv)
                        {
                            // TODO: not necessarily += 2, possibly += 1 if node is shared!
                            ++nr_bdd_nodes_per_interval[intn.interval(bdd.variable)];
                            ++nr_bdd_nodes_per_interval[intn.interval(low_var)];
                        }
                    } 
                }
                else
                {
                    assert(false); // We should have covered all cases
                }
            }
        }

        // allocate structures for holding bdd nodes
        std::vector<bdd_storage> bdd_storages(nr_intervals);
        for(size_t i=0; i<nr_intervals; ++i)
        {
            bdd_storages[i].bdd_nodes_.reserve(nr_bdd_nodes_per_interval[i]);
            bdd_storages[i].bdd_delimiters_.reserve(nr_bdds_per_interval[i]);
            bdd_storages[i].nr_variables_ = this->nr_variables_;
            assert(bdd_storages[i].bdd_delimiters_.size() == 1 && bdd_storages[i].bdd_delimiters_[0] == 0);
        }


        //two_dim_variable_array<bdd_node> split_bdd_nodes(nr_bdd_nodes_per_interval.begin(), nr_bdd_nodes_per_interval.end());
        std::fill(nr_bdd_nodes_per_interval.begin(), nr_bdd_nodes_per_interval.end(), 0);

        //two_dim_variable_array<size_t> split_bdd_delimiters(nr_bdds_per_interval.begin(), nr_bdds_per_interval.end());
        std::fill(nr_bdds_per_interval.begin(), nr_bdds_per_interval.end(), 1);
        //for(size_t i=0; i<split_bdd_delimiters.size(); ++i)
        //    split_bdd_delimiters(i,0) = 0;

        // fill split bdd nodes, record duplicated bdd variables

        // TODO: replace by tsl-robin-set
        std::unordered_set<duplicate_variable, duplicate_variable_hash> duplicated_variables;
        std::unordered_map<std::array<size_t,2>,size_t> split_bdd_node_indices; // bdd index in bdd_nodes_, interval
        for(size_t bdd_counter=0; bdd_counter<bdd_delimiters_.size()-1; ++bdd_counter)
        {
            split_bdd_node_indices.clear();
            for(auto bdd_node_counter=bdd_delimiters_[bdd_counter]; bdd_node_counter<bdd_delimiters_[bdd_counter+1]; ++bdd_node_counter)
            { 
                const bdd_node& bdd = bdd_nodes_[bdd_node_counter];
                // case (i) & (ii)
                if(!bdd.low_is_terminal() && !bdd.high_is_terminal())
                {
                    const size_t i = intn.interval(bdd.variable);
                    const bdd_node& low = bdd_nodes_[bdd.low];
                    const bdd_node& high = bdd_nodes_[bdd.high];
                    assert(low.variable == high.variable);

                    if(intn.interval(bdd.variable) == intn.interval(low.variable)) // case (i)
                    {
                        assert(split_bdd_node_indices.count({bdd.low, i}) > 0);
                        const size_t low_idx = split_bdd_node_indices.find({bdd.low, i})->second;
                        assert(split_bdd_node_indices.count({bdd.high, i}) > 0);
                        const size_t high_idx = split_bdd_node_indices.find({bdd.high, i})->second;

                        bdd_storages[i].bdd_nodes_.push_back({low_idx, high_idx, bdd.variable});
                        //split_bdd_nodes(i, nr_bdd_nodes_per_interval[i]) = {bdd.variable, low_idx, high_idx};
                        split_bdd_node_indices.insert(std::make_pair(std::array<size_t,2>{bdd_node_counter, i}, nr_bdd_nodes_per_interval[i]));
                        ++nr_bdd_nodes_per_interval[i];
                        assert(bdd_storages[i].bdd_nodes_.size() == nr_bdd_nodes_per_interval[i]);
                    }
                    else // case (ii)
                    {
                        // in interval i, low and high arcs should point to topsink
                        bdd_storages[i].bdd_nodes_.push_back({bdd_node::terminal_1, bdd_node::terminal_1, bdd.variable});
                        //split_bdd_nodes(i, nr_bdd_nodes_per_interval[i]) = {bdd.variable, bdd_node::terminal_1, bdd_node::terminal_1};
                        split_bdd_node_indices.insert(std::make_pair(std::array<size_t,2>{bdd_node_counter, i}, nr_bdd_nodes_per_interval[i]));
                        ++nr_bdd_nodes_per_interval[i];
                        assert(bdd_storages[i].bdd_nodes_.size() == nr_bdd_nodes_per_interval[i]);

                        // in next interval
                        const size_t next_i = intn.interval(bdd_nodes_[bdd.low].variable);
                        assert(i < next_i);
                        const size_t next_lo_idx = split_bdd_node_indices.find({bdd.low, next_i})->second;
                        const size_t next_hi_idx = split_bdd_node_indices.find({bdd.high, next_i})->second;
                        bdd_storages[next_i].bdd_nodes_.push_back({next_lo_idx, next_hi_idx, bdd.variable});
                        //split_bdd_nodes(next_i, nr_bdd_nodes_per_interval[next_i]) = {bdd.variable, next_lo_idx, next_hi_idx};
                        split_bdd_node_indices.insert(std::make_pair(std::array<size_t,2>{bdd_node_counter, next_i}, nr_bdd_nodes_per_interval[next_i]));
                        ++nr_bdd_nodes_per_interval[next_i]; 
                        assert(bdd_storages[next_i].bdd_nodes_.size() == nr_bdd_nodes_per_interval[next_i]);

                        duplicated_variables.insert({i, nr_bdds_per_interval[i]-1, next_i, nr_bdds_per_interval[next_i]-1});
                    }
                }
                else if(bdd.low_is_terminal() && bdd.high_is_terminal()) // case (v)
                {
                    assert(bdd.low == bdd_node::terminal_1 || bdd.high == bdd_node::terminal_1);
                    const size_t i = intn.interval(bdd.variable);
                    bdd_storages[i].bdd_nodes_.push_back({bdd.low, bdd.high, bdd.variable});
                    //split_bdd_nodes(i, nr_bdd_nodes_per_interval[i]) = {bdd.variable, bdd.low, bdd.high};
                    split_bdd_node_indices.insert(std::make_pair(std::array<size_t,2>{bdd_node_counter, i}, nr_bdd_nodes_per_interval[i])); 
                    ++nr_bdd_nodes_per_interval[i]; 
                    assert(bdd_storages[i].bdd_nodes_.size() == nr_bdd_nodes_per_interval[i]);
                }
                else if(bdd.low == bdd_node::terminal_0 || bdd.high == bdd_node::terminal_0)
                {
                    const size_t i = intn.interval(bdd.variable);
                    if(bdd.low == bdd_node::terminal_0)
                    {
                        const size_t high_var = bdd_nodes_[bdd.high].variable;
                        if(i == intn.interval(high_var)) // case (iii)
                        {
                            assert(split_bdd_node_indices.count({bdd.high,i}) > 0);
                            const size_t high_idx = split_bdd_node_indices.find({bdd.high,i})->second;
                            bdd_storages[i].bdd_nodes_.push_back({bdd_node::terminal_0, high_idx, bdd.variable});
                            //split_bdd_nodes(i, nr_bdd_nodes_per_interval[i]) = {bdd.variable, bdd_node::terminal_0, high_idx};
                            split_bdd_node_indices.insert(std::make_pair(std::array<size_t,2>{bdd_node_counter, i}, nr_bdd_nodes_per_interval[i])); 
                            ++nr_bdd_nodes_per_interval[i]; 
                            assert(bdd_storages[i].bdd_nodes_.size() == nr_bdd_nodes_per_interval[i]);
                        }
                        else // case (iv)
                        {
                            bdd_storages[i].bdd_nodes_.push_back({bdd_node::terminal_0, bdd_node::terminal_1, bdd.variable});
                            //split_bdd_nodes(i, nr_bdd_nodes_per_interval[i]) = {bdd.variable, bdd_node::terminal_0, bdd_node::terminal_1};
                            split_bdd_node_indices.insert(std::make_pair(std::array<size_t,2>{bdd_node_counter, i}, nr_bdd_nodes_per_interval[i])); 
                            ++nr_bdd_nodes_per_interval[i]; 
                            assert(bdd_storages[i].bdd_nodes_.size() == nr_bdd_nodes_per_interval[i]);

                            const size_t next_i = intn.interval(high_var);
                            assert(split_bdd_node_indices.count({bdd.high, next_i}) > 0);
                            const size_t next_high_idx = split_bdd_node_indices.find({bdd.high, next_i})->second;
                            bdd_storages[next_i].bdd_nodes_.push_back({bdd_node::terminal_0, next_high_idx, bdd.variable});
                            //split_bdd_nodes(next_i, nr_bdd_nodes_per_interval[next_i]) = {bdd.variable, bdd_node::terminal_0, next_high_idx};
                            ++nr_bdd_nodes_per_interval[next_i]; 
                            assert(bdd_storages[next_i].bdd_nodes_.size() == nr_bdd_nodes_per_interval[next_i]);

                            duplicated_variables.insert({i, nr_bdds_per_interval[i]-1, next_i, nr_bdds_per_interval[next_i]-1});
                        }
                    }
                    else
                    {
                        assert(bdd.high == bdd_node::terminal_0);
                        const size_t low_var = bdd_nodes_[bdd.low].variable;
                        if(intn.interval(bdd.variable) == intn.interval(low_var)) // case (iii)
                        {
                            assert(split_bdd_node_indices.count({bdd.low,i}) > 0);
                            const size_t low_idx = split_bdd_node_indices.find({bdd.low,i})->second;
                            bdd_storages[i].bdd_nodes_.push_back({low_idx, bdd_node::terminal_0, bdd.variable});
                            //split_bdd_nodes(i, nr_bdd_nodes_per_interval[i]) = {bdd.variable, low_idx, bdd_node::terminal_0};
                            split_bdd_node_indices.insert(std::make_pair(std::array<size_t,2>{bdd_node_counter, i}, nr_bdd_nodes_per_interval[i])); 
                            ++nr_bdd_nodes_per_interval[i]; 
                            assert(bdd_storages[i].bdd_nodes_.size() == nr_bdd_nodes_per_interval[i]);
                        }
                        else // case (iv)
                        {
                            bdd_storages[i].bdd_nodes_.push_back({bdd_node::terminal_1, bdd_node::terminal_0, bdd.variable});
                            //split_bdd_nodes(i, nr_bdd_nodes_per_interval[i]) = {bdd.variable, bdd_node::terminal_1, bdd_node::terminal_0};
                            split_bdd_node_indices.insert(std::make_pair(std::array<size_t,2>{bdd_node_counter, i}, nr_bdd_nodes_per_interval[i])); 
                            ++nr_bdd_nodes_per_interval[i]; 
                            assert(bdd_storages[i].bdd_nodes_.size() == nr_bdd_nodes_per_interval[i]);

                            const size_t next_i = intn.interval(low_var);
                            assert(split_bdd_node_indices.count({bdd.low, next_i}) > 0);
                            const size_t next_low_idx = split_bdd_node_indices.find({bdd.low, next_i})->second;
                            bdd_storages[next_i].bdd_nodes_.push_back({next_low_idx, bdd_node::terminal_0, bdd.variable});
                            //split_bdd_nodes(next_i, nr_bdd_nodes_per_interval[next_i]) = {bdd.variable, next_low_idx, bdd_node::terminal_0};
                            ++nr_bdd_nodes_per_interval[next_i]; 
                            assert(bdd_storages[next_i].bdd_nodes_.size() == nr_bdd_nodes_per_interval[next_i]);

                            duplicated_variables.insert({i, nr_bdds_per_interval[i]-1, next_i, nr_bdds_per_interval[next_i]-1});
                        }
                    } 
                }
                else
                {
                    assert(false);
                }
            }
            // go over each affected interval and set new bdd delimiter values
            active_intervals.clear();
            for(auto bdd_node_counter=bdd_delimiters_[bdd_counter]; bdd_node_counter<bdd_delimiters_[bdd_counter+1]; ++bdd_node_counter)
            {
                const auto& bdd = bdd_nodes_[bdd_node_counter];
                active_intervals.insert(intn.interval(bdd.variable));
            }
            for(const size_t i : active_intervals)
            {
                assert(bdd_storages[i].bdd_nodes_.size() == nr_bdd_nodes_per_interval[i]);
                bdd_storages[i].bdd_delimiters_.push_back(bdd_storages[i].bdd_nodes_.size());
                assert(bdd_storages[i].bdd_delimiters_.size() >= 2 && bdd_storages[i].bdd_delimiters_.back() > bdd_storages[i].bdd_delimiters_[bdd_storages[i].bdd_delimiters_.size()-2]);
                //split_bdd_delimiters(i, nr_bdds_per_interval[i]) = nr_bdd_nodes_per_interval[i];
                ++nr_bdds_per_interval[i];
            }
        }

        return {bdd_storages, duplicated_variables};
    }

}