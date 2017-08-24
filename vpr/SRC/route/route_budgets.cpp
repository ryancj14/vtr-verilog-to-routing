/* 
 * File:   route_budgets.cpp
 * Author: Jia Min Wang
 * 
 * Created on July 14, 2017, 11:34 AM
 */

#include <algorithm>
#include "vpr_context.h"
#include <fstream>
#include "vpr_error.h"
#include "globals.h"
#include "tatum/util/tatum_assert.hpp"

#include "tatum/timing_analyzers.hpp"
#include "tatum/graph_walkers.hpp"
#include "tatum/analyzer_factory.hpp"

#include "tatum/TimingGraph.hpp"
#include "tatum/TimingConstraints.hpp"
#include "tatum/TimingReporter.hpp"
#include "tatum/timing_paths.hpp"

#include "tatum/delay_calc/FixedDelayCalculator.hpp"

#include "tatum/report/graphviz_dot_writer.hpp"
#include "tatum/base/sta_util.hpp"
#include "tatum/echo_writer.hpp"
#include "tatum/TimingGraphFwd.hpp"
#include "slack_evaluation.h"
#include "tatum/TimingGraphFwd.hpp"

#include "vtr_assert.h"
#include "vtr_log.h"
#include "route_timing.h"
#include "tatum/report/TimingPathFwd.hpp"
#include "tatum/base/TimingType.hpp"
#include "timing_info.h"
#include "tatum/echo_writer.hpp"
#include "path_delay.h"
#include "net_delay.h"
#include "route_budgets.h"

#define SHORT_PATH_EXP 0.5
#define MIN_DELAY_DECREMENT 1e-9

route_budgets::route_budgets() {
    auto& cluster_ctx = g_vpr_ctx.clustering();

    num_times_congested.resize(cluster_ctx.clbs_nlist.net.size(), 0);

    min_budget_delay_ch = {NULL, 0, NULL};
    max_budget_delay_ch = {NULL, 0, NULL};
    target_budget_delay_ch = {NULL, 0, NULL};
    lower_bound_delay_ch = {NULL, 0, NULL};
    upper_bound_delay_ch = {NULL, 0, NULL};

    set = false;
}

route_budgets::~route_budgets() {
    if (set) {
        free_net_delay(delay_min_budget, &min_budget_delay_ch);
        free_net_delay(delay_max_budget, &max_budget_delay_ch);
        free_net_delay(delay_target, &target_budget_delay_ch);
        free_net_delay(delay_lower_bound, &lower_bound_delay_ch);
        free_net_delay(delay_upper_bound, &upper_bound_delay_ch);
        num_times_congested.clear();
    } else {
        vtr::free_chunk_memory(&min_budget_delay_ch);
        vtr::free_chunk_memory(&max_budget_delay_ch);
        vtr::free_chunk_memory(&target_budget_delay_ch);
        vtr::free_chunk_memory(&lower_bound_delay_ch);
        vtr::free_chunk_memory(&upper_bound_delay_ch);
    }
    set = false;
}

void route_budgets::load_route_budgets(float ** net_delay,
        std::shared_ptr<SetupTimingInfo> timing_info,
        const IntraLbPbPinLookup& pb_gpin_lookup, t_router_opts router_opts) {

    if (router_opts.routing_budgets_algorithm == DISABLE) {
        //disable budgets
        set = false;
        return;
    }

    auto& cluster_ctx = g_vpr_ctx.clustering();

    //allocate memory for budgets
    delay_min_budget = alloc_net_delay(&min_budget_delay_ch, cluster_ctx.clbs_nlist.net, cluster_ctx.clbs_nlist.net.size());
    delay_target = alloc_net_delay(&target_budget_delay_ch, cluster_ctx.clbs_nlist.net, cluster_ctx.clbs_nlist.net.size());
    delay_max_budget = alloc_net_delay(&max_budget_delay_ch, cluster_ctx.clbs_nlist.net, cluster_ctx.clbs_nlist.net.size());
    delay_lower_bound = alloc_net_delay(&lower_bound_delay_ch, cluster_ctx.clbs_nlist.net, cluster_ctx.clbs_nlist.net.size());
    delay_upper_bound = alloc_net_delay(&upper_bound_delay_ch, cluster_ctx.clbs_nlist.net, cluster_ctx.clbs_nlist.net.size());
    for (unsigned inet = 0; inet < cluster_ctx.clbs_nlist.net.size(); inet++) {
        for (unsigned ipin = 1; ipin < cluster_ctx.clbs_nlist.net[inet].pins.size(); ++ipin) {
            delay_lower_bound[inet][ipin] = 0;
            delay_upper_bound[inet][ipin] = 100e-9;
            delay_max_budget[inet][ipin] = delay_lower_bound[inet][ipin];
        }
    }

    if (router_opts.routing_budgets_algorithm == MINIMAX) {
        allocate_slack_minimax_PERT(net_delay, pb_gpin_lookup);
        calculate_delay_tagets();
    } else if (router_opts.routing_budgets_algorithm == SCALE_DELAY) {
        allocate_slack_using_delays_and_criticalities(net_delay, timing_info, pb_gpin_lookup, router_opts);
    }
    set = true;
}

void route_budgets::calculate_delay_tagets() {
    auto& cluster_ctx = g_vpr_ctx.clustering();

    for (unsigned inet = 0; inet < cluster_ctx.clbs_nlist.net.size(); inet++) {
        for (unsigned ipin = 1; ipin < cluster_ctx.clbs_nlist.net[inet].pins.size(); ipin++) {
            delay_target[inet][ipin] = min(0.5 * (delay_min_budget[inet][ipin] + delay_max_budget[inet][ipin]), delay_min_budget[inet][ipin] + 0.1e-9);
        }
    }
}

void route_budgets::allocate_slack_minimax_PERT(float ** net_delay, const IntraLbPbPinLookup& pb_gpin_lookup) {
    auto& cluster_ctx = g_vpr_ctx.clustering();

    std::shared_ptr<SetupHoldTimingInfo> timing_info = NULL;

    unsigned iteration;
    float max_budget_change;

    iteration = 0;
    max_budget_change = 900e-12;

    while (iteration < 3 || max_budget_change > 800e-12) {
        //cout << endl << "11111111111111111111111111111111111 " << max_budget_change << endl;
        timing_info = perform_sta(delay_max_budget);
        max_budget_change = allocate_slack(timing_info, delay_max_budget, net_delay, pb_gpin_lookup, SETUP);
        keep_budget_in_bounds(delay_max_budget);

        iteration++;
        if (iteration > 7)
            break;
    }

    for (unsigned inet = 0; inet < cluster_ctx.clbs_nlist.net.size(); inet++) {
        for (unsigned ipin = 1; ipin < cluster_ctx.clbs_nlist.net[inet].pins.size(); ipin++) {
            delay_min_budget[inet][ipin] = delay_max_budget[inet][ipin];
        }
    }

    iteration = 0;
    max_budget_change = 900e-12;

    while (iteration < 3 || max_budget_change > 800e-12) {
        //cout << endl << "222222222222222222222222222222222222222222222 " << max_budget_change << endl;
        timing_info = perform_sta(delay_min_budget);
        max_budget_change = allocate_slack(timing_info, delay_min_budget, net_delay, pb_gpin_lookup, HOLD);
        keep_budget_in_bounds(delay_min_budget);
        iteration++;

        if (iteration > 7)
            break;
    }
    keep_min_below_max_budget();

    float bottom_range = -1e-9;
    timing_info = perform_sta(delay_min_budget);
    //cout << endl << "333333333333333333333333333333333333333333 " << endl;
    allocate_slack(timing_info, delay_min_budget, net_delay, pb_gpin_lookup, HOLD);
    for (unsigned inet = 0; inet < cluster_ctx.clbs_nlist.net.size(); inet++) {
        for (unsigned ipin = 1; ipin < cluster_ctx.clbs_nlist.net[inet].pins.size(); ipin++) {
            delay_min_budget[inet][ipin] = max(delay_min_budget[inet][ipin], bottom_range);
        }
    }
    keep_min_below_max_budget();
}

void route_budgets::keep_budget_in_bounds(float ** temp_budgets) {
    auto& cluster_ctx = g_vpr_ctx.clustering();
    for (unsigned inet = 0; inet < cluster_ctx.clbs_nlist.net.size(); inet++) {
        for (unsigned ipin = 1; ipin < cluster_ctx.clbs_nlist.net[inet].pins.size(); ipin++) {
            temp_budgets[inet][ipin] = max(temp_budgets[inet][ipin], delay_lower_bound[inet][ipin]);
            temp_budgets[inet][ipin] = min(temp_budgets[inet][ipin], delay_upper_bound[inet][ipin]);
        }
    }
}

void route_budgets::keep_min_below_max_budget() {
    auto& cluster_ctx = g_vpr_ctx.clustering();
    for (unsigned inet = 0; inet < cluster_ctx.clbs_nlist.net.size(); inet++) {
        for (unsigned ipin = 1; ipin < cluster_ctx.clbs_nlist.net[inet].pins.size(); ipin++) {
            if (delay_min_budget[inet][ipin] > delay_max_budget[inet][ipin]) {
                delay_min_budget[inet][ipin] = delay_max_budget[inet][ipin];
            }
        }
    }
}

float route_budgets::allocate_slack(std::shared_ptr<SetupHoldTimingInfo> timing_info, float ** temp_budgets,
        float ** net_delay, const IntraLbPbPinLookup& pb_gpin_lookup, analysis_type analysis_type) {
    auto& cluster_ctx = g_vpr_ctx.clustering();
    auto& atom_ctx = g_vpr_ctx.atom();

    std::shared_ptr<const tatum::SetupHoldTimingAnalyzer> timing_analyzer = timing_info->setup_hold_analyzer();
    float total_path_delay = 0;
    float path_slack;
    float max_budget_change = 0;
    for (unsigned inet = 0; inet < cluster_ctx.clbs_nlist.net.size(); inet++) {
        for (unsigned ipin = 1; ipin < cluster_ctx.clbs_nlist.net[inet].pins.size(); ipin++) {
            total_path_delay = 0;
            const t_net_pin& net_pin = cluster_ctx.clbs_nlist.net[inet].pins[ipin];
            std::vector<AtomPinId> atom_pins = find_clb_pin_connected_atom_pins(net_pin.block, net_pin.block_pin, pb_gpin_lookup);
            for (const AtomPinId atom_pin : atom_pins) {
                tatum::NodeId timing_node = atom_ctx.lookup.atom_pin_tnode(atom_pin);

                float new_total_path_delay = get_total_path_delay(timing_analyzer, analysis_type, timing_node);
                if (new_total_path_delay == -1) {
                    continue;
                }
                total_path_delay = max(new_total_path_delay, total_path_delay);
            }

            if (total_path_delay == 0) {
                temp_budgets[inet][ipin] = 0;
                continue;
            }

            //calculate slack
            if (analysis_type == HOLD) {
                path_slack = calculate_clb_pin_slack(inet, ipin, timing_info, pb_gpin_lookup, HOLD);
            } else {
                path_slack = calculate_clb_pin_slack(inet, ipin, timing_info, pb_gpin_lookup, SETUP);
            }

            if (analysis_type == HOLD) {
                temp_budgets[inet][ipin] = -1 * net_delay[inet][ipin] * path_slack / total_path_delay;
                //                if (inet == 14) {
                //                    cout << "HOLD pin " << ipin << " net delay " << net_delay[inet][ipin] << " total path delay " << total_path_delay
                //                            << " slack " << path_slack << " temp_budgets " << temp_budgets[inet][ipin] << endl;
                //                }
            } else {
                temp_budgets[inet][ipin] = net_delay[inet][ipin] * path_slack / total_path_delay;

                //                if (inet == 14) {
                //                    cout << "SETUP pin " << ipin << " net delay " << net_delay[inet][ipin] << " total path delay " << total_path_delay
                //                            << " slack " << path_slack << " temp_budgets " << temp_budgets[inet][ipin] << endl;
                //                }
            }
            max_budget_change = max(max_budget_change, net_delay[inet][ipin] * path_slack / total_path_delay);
        }
    }
    return max_budget_change;
}

float route_budgets::calculate_clb_pin_slack(int inet, int ipin, std::shared_ptr<SetupHoldTimingInfo> timing_info,
        const IntraLbPbPinLookup& pb_gpin_lookup, analysis_type type) {
    auto& cluster_ctx = g_vpr_ctx.clustering();

    const t_net_pin& net_pin = cluster_ctx.clbs_nlist.net[inet].pins[ipin];

    //There may be multiple atom netlist pins connected to this CLB pin
    std::vector<AtomPinId> atom_pins = find_clb_pin_connected_atom_pins(net_pin.block, net_pin.block_pin, pb_gpin_lookup);

    //Take the minimum of the atom pin slack as the CLB pin slack
    float clb_min_slack = delay_upper_bound[inet][ipin];
    for (const AtomPinId atom_pin : atom_pins) {
        if (timing_info->setup_pin_slack(atom_pin) == std::numeric_limits<float>::infinity()) {
            continue;
        } else {
            if (type == HOLD) {
                clb_min_slack = std::min(clb_min_slack, timing_info->hold_pin_slack(atom_pin));
            } else {
                clb_min_slack = std::min(clb_min_slack, timing_info->setup_pin_slack(atom_pin));
            }
        }
    }
    atom_pins.clear();

    return clb_min_slack;
}

float route_budgets::get_total_path_delay(std::shared_ptr<const tatum::SetupHoldTimingAnalyzer> timing_analyzer,
        analysis_type analysis_type, tatum::NodeId timing_node) {

    auto arrival_tags = timing_analyzer->setup_tags(timing_node, tatum::TagType::DATA_ARRIVAL);
    auto required_tags = timing_analyzer->setup_tags(timing_node, tatum::TagType::DATA_REQUIRED);

    if (analysis_type == HOLD) {
        arrival_tags = timing_analyzer->hold_tags(timing_node, tatum::TagType::DATA_ARRIVAL);
        required_tags = timing_analyzer->hold_tags(timing_node, tatum::TagType::DATA_REQUIRED);
    }

    if (arrival_tags.empty() || required_tags.empty()) {
        return -1;
    }

    auto min_arrival_tag_iter = find_minimum_tag(arrival_tags);
    auto max_required_tag_iter = find_maximum_tag(required_tags);

    tatum::NodeId sink_node = max_required_tag_iter->origin_node();
    if (sink_node == tatum::NodeId::INVALID()) {
        return -1;
    }

    auto sink_node_tags = timing_analyzer->setup_tags(sink_node, tatum::TagType::DATA_REQUIRED);

    if (analysis_type == HOLD) {
        sink_node_tags = timing_analyzer->hold_tags(sink_node, tatum::TagType::DATA_REQUIRED);
    }

    if (sink_node_tags.empty()) {
        return -1;
    }

    auto min_sink_node_tag_iter = find_minimum_tag(sink_node_tags);

    if (max_required_tag_iter != required_tags.end() && min_arrival_tag_iter != arrival_tags.end()
            && max_required_tag_iter != sink_node_tags.end()) {

        float final_required_time = min_sink_node_tag_iter->time().value();

        float future_path_delay = final_required_time - max_required_tag_iter->time().value();
        float past_path_delay = min_arrival_tag_iter->time().value();

        //cout << past_path_delay << " " << future_path_delay << endl;

        return past_path_delay + future_path_delay;
    } else {
        return -1;
    }
}

void route_budgets::allocate_slack_using_delays_and_criticalities(float ** net_delay,
        std::shared_ptr<SetupTimingInfo> timing_info,
        const IntraLbPbPinLookup& pb_gpin_lookup, t_router_opts router_opts) {
    auto& cluster_ctx = g_vpr_ctx.clustering();
    float pin_criticality;
    for (unsigned inet = 0; inet < cluster_ctx.clbs_nlist.net.size(); inet++) {
        for (unsigned ipin = 1; ipin < cluster_ctx.clbs_nlist.net[inet].pins.size(); ipin++) {
            pin_criticality = calculate_clb_net_pin_criticality(*timing_info, pb_gpin_lookup, inet, ipin);

            /* Pin criticality is between 0 and 1. 
             * Shift it downwards by 1 - max_criticality (max_criticality is 0.99 by default, 
             * so shift down by 0.01) and cut off at 0.  This means that all pins with small 
             * criticalities (<0.01) get criticality 0 and are ignored entirely, and everything 
             * else becomes a bit less critical. This effect becomes more pronounced if 
             * max_criticality is set lower. */
            // VTR_ASSERT(pin_criticality[ipin] > -0.01 && pin_criticality[ipin] < 1.01);
            pin_criticality = max(pin_criticality - (1.0 - router_opts.max_criticality), 0.0);

            /* Take pin criticality to some power (1 by default). */
            pin_criticality = pow(pin_criticality, router_opts.criticality_exp);

            /* Cut off pin criticality at max_criticality. */
            pin_criticality = min(pin_criticality, router_opts.max_criticality);

            delay_min_budget[inet][ipin] = 0;
            delay_lower_bound[inet][ipin] = 0;
            delay_upper_bound[inet][ipin] = 100e-9;

            if (pin_criticality == 0) {
                //prevent invalid division
                delay_max_budget[inet][ipin] = delay_upper_bound[inet][ipin];
            } else {
                delay_max_budget[inet][ipin] = min(net_delay[inet][ipin] / pin_criticality, delay_upper_bound[inet][ipin]);
            }

            VTR_ASSERT_MSG(delay_max_budget[inet][ipin] >= delay_min_budget[inet][ipin]
                    && delay_lower_bound[inet][ipin] <= delay_min_budget[inet][ipin]
                    && delay_upper_bound[inet][ipin] >= delay_max_budget[inet][ipin]
                    && delay_upper_bound[inet][ipin] >= delay_lower_bound[inet][ipin],
                    "Delay budgets do not fit in delay bounds");

            //Use RCV algorithm for delay target
            //Tend towards minimum to consider short path timing delay more
            delay_target[inet][ipin] = min(0.5 * (delay_min_budget[inet][ipin] + delay_max_budget[inet][ipin]), delay_min_budget[inet][ipin] + 0.1e-9);

        }
    }
}

std::shared_ptr<SetupHoldTimingInfo> route_budgets::perform_sta(float ** temp_budgets) {
    auto& atom_ctx = g_vpr_ctx.atom();

    std::shared_ptr<RoutingDelayCalculator> routing_delay_calc =
            std::make_shared<RoutingDelayCalculator>(atom_ctx.nlist, atom_ctx.lookup, temp_budgets);

    std::shared_ptr<SetupHoldTimingInfo> timing_info = make_setup_hold_timing_info(routing_delay_calc);
    timing_info->update();

    return timing_info;
}

void route_budgets::update_congestion_times(int inet) {

    num_times_congested[inet]++;
}

void route_budgets::not_congested_this_iteration(int inet) {

    num_times_congested[inet] = 0;
}

void route_budgets::lower_budgets() {
    auto& cluster_ctx = g_vpr_ctx.clustering();

    for (unsigned inet = 0; inet < cluster_ctx.clbs_nlist.net.size(); inet++) {
        if (num_times_congested[inet] >= 3) {
            for (unsigned ipin = 1; ipin < cluster_ctx.clbs_nlist.net[inet].pins.size(); ipin++) {
                if (delay_min_budget[inet][ipin] - delay_lower_bound[inet][ipin] >= 1e-9) {
                    delay_min_budget[inet][ipin] = delay_min_budget[inet][ipin] - MIN_DELAY_DECREMENT;
                }
            }
        }
    }
}

float route_budgets::get_delay_target(int inet, int ipin) {
    //cannot get delay from a source
    VTR_ASSERT(ipin);

    return delay_target[inet][ipin];
}

float route_budgets::get_min_delay_budget(int inet, int ipin) {
    //cannot get delay from a source
    VTR_ASSERT(ipin);

    return delay_min_budget[inet][ipin];
}

float route_budgets::get_max_delay_budget(int inet, int ipin) {
    //cannot get delay from a source
    VTR_ASSERT(ipin);

    return delay_max_budget[inet][ipin];
}

float route_budgets::get_crit_short_path(int inet, int ipin) {
    //cannot get delay from a source
    VTR_ASSERT(ipin);
    if (delay_target[inet][ipin] == 0) {

        return 0;
    }
    return pow(((delay_target[inet][ipin] - delay_lower_bound[inet][ipin]) / delay_target[inet][ipin]), SHORT_PATH_EXP);
}

void route_budgets::print_route_budget() {
    auto& cluster_ctx = g_vpr_ctx.clustering();
    unsigned inet, ipin;
    fstream fp;
    fp.open("route_budget.txt", fstream::out | fstream::trunc);

    /* Prints out general info for easy error checking*/
    if (!fp.is_open() || !fp.good()) {
        vpr_throw(VPR_ERROR_OTHER, __FILE__, __LINE__,
                "could not open \"route_budget.txt\" for generating route budget file\n");
    }

    fp << "Minimum Delay Budgets:" << endl;
    for (inet = 0; inet < cluster_ctx.clbs_nlist.net.size(); inet++) {
        fp << endl << "Net: " << inet << "            ";
        for (ipin = 1; ipin < cluster_ctx.clbs_nlist.net[inet].pins.size(); ipin++) {
            fp << delay_min_budget[inet][ipin] << " ";
        }
    }

    fp << endl << endl << "Maximum Delay Budgets:" << endl;
    for (inet = 0; inet < cluster_ctx.clbs_nlist.net.size(); inet++) {
        fp << endl << "Net: " << inet << "            ";
        for (ipin = 1; ipin < cluster_ctx.clbs_nlist.net[inet].pins.size(); ipin++) {
            fp << delay_max_budget[inet][ipin] << " ";
        }
    }

    fp << endl << endl << "Target Delay Budgets:" << endl;

    for (inet = 0; inet < cluster_ctx.clbs_nlist.net.size(); inet++) {
        fp << endl << "Net: " << inet << "            ";
        for (ipin = 1; ipin < cluster_ctx.clbs_nlist.net[inet].pins.size(); ipin++) {
            fp << delay_target[inet][ipin] << " ";
        }
    }

    fp << endl << endl << "Delay lower_bound:" << endl;
    for (inet = 0; inet < cluster_ctx.clbs_nlist.net.size(); inet++) {
        fp << endl << "Net: " << inet << "            ";
        for (ipin = 1; ipin < cluster_ctx.clbs_nlist.net[inet].pins.size(); ipin++) {
            fp << delay_lower_bound[inet][ipin] << " ";
        }
    }

    fp << endl << endl << "Target Delay Budgets:" << endl;
    for (inet = 0; inet < cluster_ctx.clbs_nlist.net.size(); inet++) {
        fp << endl << "Net: " << inet << "            ";
        for (ipin = 1; ipin < cluster_ctx.clbs_nlist.net[inet].pins.size(); ipin++) {

            fp << delay_upper_bound[inet][ipin] << " ";
        }
    }

    fp.close();
}

void route_budgets::print_temporary_budgets_to_file(float ** temp_budgets) {
    auto& cluster_ctx = g_vpr_ctx.clustering();
    unsigned inet, ipin;
    fstream fp;
    fp.open("temporary_budgets.txt", fstream::out | fstream::trunc);


    fp << "Temporary Budgets:" << endl;
    for (inet = 0; inet < cluster_ctx.clbs_nlist.net.size(); inet++) {
        fp << endl << "Net: " << inet << "            ";
        for (ipin = 1; ipin < cluster_ctx.clbs_nlist.net[inet].pins.size(); ipin++) {

            fp << temp_budgets[inet][ipin] << " ";
        }
    }
}

bool route_budgets::if_set() {
    return set;
}