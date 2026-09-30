/* RACCOON V2 - simulator scenarios.  See rc_sim_scenarios.c. */
#ifndef RC_SIM_SCENARIOS_H
#define RC_SIM_SCENARIOS_H

#ifdef __cplusplus
extern "C" {
#endif

void sim_all(void);
void sim_unverified_tune_refuses_to_arm(void);
void sim_cold_start_settles_to_idle(void);
void sim_trigger_loss_inhibits_control(void);
void sim_wide_open_throttle_moves_and_stops_at_the_limiter(void);
void sim_lean_condition_produces_a_cut(void);
void sim_closed_loop_corrects_in_the_right_direction(void);
void sim_sensor_failures_degrade_not_crash(void);
void sim_no_fuel_without_a_trustworthy_crank_position(void);
void sim_oil_pressure_loss_shuts_down(void);

#ifdef __cplusplus
}
#endif
#endif /* RC_SIM_SCENARIOS_H */
