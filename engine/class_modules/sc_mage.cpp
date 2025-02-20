// ==========================================================================
// Dedmonwakeen's DPS-DPM Simulator.
// Send questions to natehieter@gmail.com
// ==========================================================================

#include "simulationcraft.hpp"

namespace {

// ==========================================================================
// Mage
// ==========================================================================

// Forward declarations
struct mage_t;

namespace pets {
  namespace water_elemental {
    struct water_elemental_pet_t;
  }
}

template <typename Action, typename Actor, typename... Args>
action_t* get_action( const std::string& name, Actor* actor, Args&&... args )
{
  action_t* a = actor->find_action( name );
  if ( !a )
    a = new Action( name, actor, std::forward<Args>( args )... );
  assert( dynamic_cast<Action*>( a ) && a->name_str == name && a->background );
  return a;
}

enum frozen_type_e
{
  FROZEN_WINTERS_CHILL = 0,
  FROZEN_FINGERS_OF_FROST,
  FROZEN_ROOT,

  FROZEN_NONE,
  FROZEN_MAX
};

enum frozen_flag_e
{
  FF_WINTERS_CHILL    = 1 << FROZEN_WINTERS_CHILL,
  FF_FINGERS_OF_FROST = 1 << FROZEN_FINGERS_OF_FROST,
  FF_ROOT             = 1 << FROZEN_ROOT
};

enum rotation_type_e
{
  ROTATION_STANDARD,
  ROTATION_NO_ICE_LANCE,
  ROTATION_FROZEN_ORB
};

struct state_switch_t
{
private:
  bool state;
  timespan_t last_enable;
  timespan_t last_disable;

public:
  state_switch_t()
  {
    reset();
  }

  bool enable( timespan_t now )
  {
    if ( last_enable == now )
      return false;

    state = true;
    last_enable = now;
    return true;
  }

  bool disable( timespan_t now )
  {
    if ( last_disable == now )
      return false;

    state = false;
    last_disable = now;
    return true;
  }

  bool on() const
  {
    return state;
  }

  timespan_t duration( timespan_t now ) const
  {
    return state ? now - last_enable : 0_ms;
  }

  void reset()
  {
    state        = false;
    last_enable  = timespan_t::min();
    last_disable = timespan_t::min();
  }
};

/// Icicle container object, contains a timestamp and its corresponding icicle data!
struct icicle_tuple_t
{
  action_t* action;
  event_t*  expiration;
};

struct mage_td_t : public actor_target_data_t
{
  struct dots_t
  {
    dot_t* nether_tempest;
  } dots;

  struct debuffs_t
  {
    buff_t* frozen;
    buff_t* winters_chill;
    buff_t* touch_of_the_magi;

    // Azerite
    buff_t* packed_ice;
  } debuffs;

  mage_td_t( player_t* target, mage_t* mage );
};

struct buff_stack_benefit_t
{
  const buff_t* buff;
  std::vector<benefit_t*> buff_stack_benefit;

  buff_stack_benefit_t( const buff_t* _buff, const std::string& prefix ) :
    buff( _buff ),
    buff_stack_benefit()
  {
    for ( int i = 0; i <= buff->max_stack(); i++ )
    {
      buff_stack_benefit.push_back( buff->player->get_benefit(
        prefix + " " + buff->data().name_cstr() + " " + util::to_string( i ) ) );
    }
  }

  void update()
  {
    auto stack = as<unsigned>( buff->check() );
    for ( unsigned i = 0; i < buff_stack_benefit.size(); ++i )
      buff_stack_benefit[ i ]->update( i == stack );
  }
};

struct cooldown_reduction_data_t
{
  const cooldown_t* cd;

  luxurious_sample_data_t* effective;
  luxurious_sample_data_t* wasted;

  cooldown_reduction_data_t( const cooldown_t* cooldown, const std::string& name ) :
    cd( cooldown )
  {
    effective = cd->player->get_sample_data( name + " effective cooldown reduction" );
    wasted    = cd->player->get_sample_data( name + " wasted cooldown reduction" );
  }

  void add( timespan_t reduction )
  {
    timespan_t remaining = cd->recharge_event
      ? cd->current_charge_remains() + ( cd->charges - cd->current_charge - 1 ) * cooldown_t::cooldown_duration( cd )
      : cd->remains();

    double reduction_sec = -reduction.total_seconds();
    double remaining_sec = remaining.total_seconds();
    double effective_sec = std::min( reduction_sec, remaining_sec );
    effective->add( effective_sec );
    wasted->add( reduction_sec - effective_sec );
  }
};

struct cooldown_waste_data_t : private noncopyable
{
  const cooldown_t* cd;
  double buffer;

  extended_sample_data_t normal;
  extended_sample_data_t cumulative;

  cooldown_waste_data_t( const cooldown_t* cooldown, bool simple = true ) :
    cd( cooldown ),
    buffer(),
    normal( cd->name_str + " cooldown waste", simple ),
    cumulative( cd->name_str + " cumulative cooldown waste", simple )
  { }

  void add( timespan_t cd_override = timespan_t::min(), timespan_t time_to_execute = 0_ms )
  {
    if ( cd_override == 0_ms || ( cd_override < 0_ms && cd->duration <= 0_ms ) )
      return;

    if ( cd->ongoing() )
    {
      normal.add( 0.0 );
    }
    else
    {
      double wasted = ( cd->sim.current_time() - cd->last_charged ).total_seconds();

      // Waste caused by execute time is unavoidable for single charge spells, don't count it.
      if ( cd->charges == 1 )
        wasted -= time_to_execute.total_seconds();

      normal.add( wasted );
      buffer += wasted;
    }
  }

  bool active() const
  {
    return normal.count() > 0 && cumulative.sum() > 0;
  }

  void merge( const cooldown_waste_data_t& other )
  {
    normal.merge( other.normal );
    cumulative.merge( other.cumulative );
  }

  void analyze()
  {
    normal.analyze();
    cumulative.analyze();
  }

  void datacollection_begin()
  {
    buffer = 0.0;
  }

  void datacollection_end()
  {
    if ( !cd->ongoing() )
      buffer += ( cd->sim.current_time() - cd->last_charged ).total_seconds();

    cumulative.add( buffer );
  }
};

template <size_t N>
struct effect_source_t : private noncopyable
{
  const std::string name_str;
  std::array<simple_sample_data_t, N> counts;
  std::array<int, N> iteration_counts;

  effect_source_t( const std::string& name ) :
    name_str( name ),
    counts(),
    iteration_counts()
  { }

  void occur( size_t type )
  {
    assert( type < N );
    iteration_counts[ type ]++;
  }

  double count( size_t type ) const
  {
    assert( type < N );
    return counts[ type ].pretty_mean();
  }

  double count_total() const
  {
    double res = 0.0;
    for ( const auto& c : counts )
      res += c.pretty_mean();
    return res;
  }

  bool active() const
  {
    return count_total() > 0.0;
  }

  void merge( const effect_source_t& other )
  {
    for ( size_t i = 0; i < counts.size(); i++ )
      counts[ i ].merge( other.counts[ i ] );
  }

  void datacollection_begin()
  {
    range::fill( iteration_counts, 0 );
  }

  void datacollection_end()
  {
    for ( size_t i = 0; i < counts.size(); i++ )
      counts[ i ].add( as<double>( iteration_counts[ i ] ) );
  }
};

typedef effect_source_t<FROZEN_MAX> shatter_source_t;

struct mage_t : public player_t
{
public:
  // Icicles
  std::vector<icicle_tuple_t> icicles;
  event_t* icicle_event;

  struct icicles_t
  {
    action_t* frostbolt;
    action_t* flurry;
    action_t* lucid_dreams;
  } icicle;

  // Ignite
  action_t* ignite;
  event_t* ignite_spread_event;

  // Time Anomaly
  event_t* time_anomaly_tick_event;

  // Active
  player_t* last_bomb_target;
  player_t* last_frostbolt_target;

  // State switches for rotation selection
  state_switch_t burn_phase;

  // Ground AoE tracking
  std::map<std::string, timespan_t> ground_aoe_expiration;

  // Miscellaneous
  double distance_from_rune;
  double lucid_dreams_refund;
  double strive_for_perfection_multiplier;
  double vision_of_perfection_multiplier;

  // Data collection
  auto_dispose<std::vector<cooldown_waste_data_t*> > cooldown_waste_data_list;
  auto_dispose<std::vector<shatter_source_t*> > shatter_source_list;

  // Cached actions
  struct actions_t
  {
    action_t* arcane_assault;
    action_t* conflagration_flare_up;
    action_t* glacial_assault;
    action_t* living_bomb_dot;
    action_t* living_bomb_dot_spread;
    action_t* living_bomb_explosion;
    action_t* meteor_burn;
    action_t* meteor_impact;
    action_t* touch_of_the_magi;
  } action;

  // Benefits
  struct benefits_t
  {
    struct arcane_charge_benefits_t
    {
      std::unique_ptr<buff_stack_benefit_t> arcane_barrage;
      std::unique_ptr<buff_stack_benefit_t> arcane_blast;
      std::unique_ptr<buff_stack_benefit_t> nether_tempest;
    } arcane_charge;

    struct blaster_master_benefits_t
    {
      std::unique_ptr<buff_stack_benefit_t> combustion;
      std::unique_ptr<buff_stack_benefit_t> rune_of_power;
      std::unique_ptr<buff_stack_benefit_t> searing_touch;
    } blaster_master;
  } benefits;

  // Buffs
  struct buffs_t
  {
    // Arcane
    buff_t* arcane_charge;
    buff_t* arcane_power;
    buff_t* clearcasting;
    buff_t* clearcasting_channel; // Hidden buff which governs tick and channel time
    buff_t* evocation;
    buff_t* presence_of_mind;

    buff_t* arcane_familiar;
    buff_t* chrono_shift;
    buff_t* rule_of_threes;


    // Fire
    buff_t* combustion;
    buff_t* enhanced_pyrotechnics;
    buff_t* heating_up;
    buff_t* hot_streak;

    buff_t* frenetic_speed;
    buff_t* pyroclasm;


    // Frost
    buff_t* brain_freeze;
    buff_t* fingers_of_frost;
    buff_t* icicles;
    buff_t* icy_veins;

    buff_t* bone_chilling;
    buff_t* chain_reaction;
    buff_t* freezing_rain;
    buff_t* ice_floes;
    buff_t* ray_of_frost;


    // Shared
    buff_t* incanters_flow;
    buff_t* rune_of_power;


    // Azerite
    buff_t* arcane_pummeling;
    buff_t* brain_storm;

    buff_t* blaster_master;
    buff_t* firemind;
    buff_t* flames_of_alacrity;
    buff_t* wildfire;

    buff_t* frigid_grasp;
    buff_t* tunnel_of_ice;


    // Miscellaneous Buffs
    buff_t* gbow;
    buff_t* shimmer;
  } buffs;

  // Cooldowns
  struct cooldowns_t
  {
    cooldown_t* combustion;
    cooldown_t* cone_of_cold;
    cooldown_t* fire_blast;
    cooldown_t* frost_nova;
    cooldown_t* frozen_orb;
    cooldown_t* presence_of_mind;
  } cooldowns;

  // Gains
  struct gains_t
  {
    gain_t* gbow;
    gain_t* evocation;
    gain_t* lucid_dreams;
  } gains;

  // Options
  struct options_t
  {
    timespan_t firestarter_time = 0_ms;
    timespan_t frozen_duration = 1.0_s;
    timespan_t scorch_delay = 15_ms;
    int gbow_count = 0;
    bool allow_shimmer_lance = false;
    rotation_type_e rotation = ROTATION_STANDARD;
    double lucid_dreams_proc_chance_arcane = 0.15;
    double lucid_dreams_proc_chance_fire = 0.1;
    double lucid_dreams_proc_chance_frost = 0.075;
  } options;

  // Pets
  struct pets_t
  {
    pets::water_elemental::water_elemental_pet_t* water_elemental = nullptr;
    std::vector<pet_t*> mirror_images;
  } pets;

  // Procs
  struct procs_t
  {
    proc_t* heating_up_generated;         // Crits without HU/HS
    proc_t* heating_up_removed;           // Non-crits with HU >200ms after application
    proc_t* heating_up_ib_converted;      // IBs used on HU
    proc_t* hot_streak;                   // Total HS generated
    proc_t* hot_streak_pyromaniac;        // Total HS from Pyromaniac
    proc_t* hot_streak_spell;             // HU/HS spell impacts
    proc_t* hot_streak_spell_crit;        // HU/HS spell crits
    proc_t* hot_streak_spell_crit_wasted; // HU/HS spell crits with HS

    proc_t* ignite_applied;    // Direct ignite applications
    proc_t* ignite_spread;     // Spread events
    proc_t* ignite_new_spread; // Spread to new target
    proc_t* ignite_overwrite;  // Spread to target with existing ignite

    proc_t* brain_freeze;
    proc_t* brain_freeze_used;
    proc_t* fingers_of_frost;
    proc_t* fingers_of_frost_wasted;
  } procs;

  struct shuffled_rngs_t
  {
    shuffled_rng_t* time_anomaly;
  } shuffled_rng;

  // Sample data
  struct sample_data_t
  {
    std::unique_ptr<cooldown_reduction_data_t> blizzard;
    std::unique_ptr<extended_sample_data_t> icy_veins_duration;
    std::unique_ptr<extended_sample_data_t> burn_duration_history;
    std::unique_ptr<extended_sample_data_t> burn_initial_mana;
  } sample_data;

  // Specializations
  struct specializations_t
  {
    // Arcane
    const spell_data_t* arcane_barrage_2;
    const spell_data_t* arcane_charge;
    const spell_data_t* arcane_mage;
    const spell_data_t* clearcasting;
    const spell_data_t* evocation_2;
    const spell_data_t* savant;

    // Fire
    const spell_data_t* critical_mass;
    const spell_data_t* critical_mass_2;
    const spell_data_t* enhanced_pyrotechnics;
    const spell_data_t* fire_blast_2;
    const spell_data_t* fire_blast_3;
    const spell_data_t* fire_mage;
    const spell_data_t* hot_streak;
    const spell_data_t* ignite;

    // Frost
    const spell_data_t* brain_freeze;
    const spell_data_t* brain_freeze_2;
    const spell_data_t* blizzard_2;
    const spell_data_t* fingers_of_frost;
    const spell_data_t* frost_mage;
    const spell_data_t* icicles;
    const spell_data_t* shatter;
    const spell_data_t* shatter_2;
  } spec;

  // State
  struct state_t
  {
    bool brain_freeze_active;
    bool fingers_of_frost_active;
  } state;

  // Talents
  struct talents_list_t
  {
    // Tier 15
    const spell_data_t* amplification;
    const spell_data_t* rule_of_threes;
    const spell_data_t* arcane_familiar;
    const spell_data_t* firestarter;
    const spell_data_t* pyromaniac;
    const spell_data_t* searing_touch;
    const spell_data_t* bone_chilling;
    const spell_data_t* lonely_winter;
    const spell_data_t* ice_nova;

    // Tier 30
    const spell_data_t* shimmer;
    const spell_data_t* mana_shield; // NYI
    const spell_data_t* slipstream;
    const spell_data_t* blazing_soul; // NYI
    const spell_data_t* blast_wave;
    const spell_data_t* glacial_insulation; // NYI
    const spell_data_t* ice_floes;

    // Tier 45
    const spell_data_t* incanters_flow;
    const spell_data_t* mirror_image;
    const spell_data_t* rune_of_power;

    // Tier 60
    const spell_data_t* resonance;
    const spell_data_t* charged_up;
    const spell_data_t* supernova;
    const spell_data_t* flame_on;
    const spell_data_t* alexstraszas_fury;
    const spell_data_t* phoenix_flames;
    const spell_data_t* frozen_touch;
    const spell_data_t* chain_reaction;
    const spell_data_t* ebonbolt;

    // Tier 75
    const spell_data_t* ice_ward;
    const spell_data_t* ring_of_frost; // NYI
    const spell_data_t* chrono_shift;
    const spell_data_t* frenetic_speed;
    const spell_data_t* frigid_winds; // NYI

    // Tier 90
    const spell_data_t* reverberate;
    const spell_data_t* touch_of_the_magi;
    const spell_data_t* nether_tempest;
    const spell_data_t* flame_patch;
    const spell_data_t* conflagration;
    const spell_data_t* living_bomb;
    const spell_data_t* freezing_rain;
    const spell_data_t* splitting_ice;
    const spell_data_t* comet_storm;

    // Tier 100
    const spell_data_t* overpowered;
    const spell_data_t* time_anomaly;
    const spell_data_t* arcane_orb;
    const spell_data_t* kindling;
    const spell_data_t* pyroclasm;
    const spell_data_t* meteor;
    const spell_data_t* thermal_void;
    const spell_data_t* ray_of_frost;
    const spell_data_t* glacial_spike;
  } talents;

  // Azerite Powers
  struct azerite_powers_t
  {
    // Arcane
    azerite_power_t arcane_pressure;
    azerite_power_t arcane_pummeling;
    azerite_power_t brain_storm;
    azerite_power_t equipoise;
    azerite_power_t explosive_echo;
    azerite_power_t galvanizing_spark;

    // Fire
    azerite_power_t blaster_master;
    azerite_power_t duplicative_incineration;
    azerite_power_t firemind;
    azerite_power_t flames_of_alacrity;
    azerite_power_t trailing_embers;
    azerite_power_t wildfire;

    // Frost
    azerite_power_t flash_freeze;
    azerite_power_t frigid_grasp;
    azerite_power_t glacial_assault;
    azerite_power_t packed_ice;
    azerite_power_t tunnel_of_ice;
    azerite_power_t whiteout;
  } azerite;

  struct uptimes_t
  {
    uptime_t* burn_phase;
    uptime_t* conserve_phase;
  } uptime;

public:
  mage_t( sim_t* sim, const std::string& name, race_e r = RACE_NONE );

  // Character Definition
  void        init_spells() override;
  void        init_base_stats() override;
  void        create_buffs() override;
  void        create_options() override;
  void        init_assessors() override;
  void        init_action_list() override;
  std::string default_potion() const override;
  std::string default_flask() const override;
  std::string default_food() const override;
  std::string default_rune() const override;
  void        init_gains() override;
  void        init_procs() override;
  void        init_benefits() override;
  void        init_uptimes() override;
  void        init_rng() override;
  void        init_finished() override;
  void        invalidate_cache( cache_e ) override;
  void        init_resources( bool ) override;
  void        recalculate_resource_max( resource_e ) override;
  void        reset() override;
  expr_t*     create_expression( const std::string& ) override;
  expr_t*     create_action_expression( action_t&, const std::string& ) override;
  action_t*   create_action( const std::string&, const std::string& ) override;
  void        create_actions() override;
  void        create_pets() override;
  resource_e  primary_resource() const override { return RESOURCE_MANA; }
  role_e      primary_role() const override { return ROLE_SPELL; }
  stat_e      convert_hybrid_stat( stat_e ) const override;
  double      resource_regen_per_second( resource_e ) const override;
  double      composite_player_pet_damage_multiplier( const action_state_t* ) const override;
  double      composite_spell_crit_chance() const override;
  double      composite_rating_multiplier( rating_e ) const override;
  double      composite_spell_haste() const override;
  double      matching_gear_multiplier( attribute_e ) const override;
  void        update_movement( timespan_t ) override;
  void        teleport( double, timespan_t ) override;
  double      passive_movement_modifier() const override;
  void        arise() override;
  void        combat_begin() override;
  void        combat_end() override;
  std::string create_profile( save_e ) override;
  void        copy_from( player_t* ) override;
  void        merge( player_t& ) override;
  void        analyze( sim_t& ) override;
  void        datacollection_begin() override;
  void        datacollection_end() override;
  void        regen( timespan_t ) override;
  void        moving() override;
  void        vision_of_perfection_proc() override;

  target_specific_t<mage_td_t> target_data;

  mage_td_t* get_target_data( player_t* target ) const override
  {
    mage_td_t*& td = target_data[ target ];
    if ( !td )
      td = new mage_td_t( target, const_cast<mage_t*>( this ) );
    return td;
  }

  // Public mage functions:
  cooldown_waste_data_t* get_cooldown_waste_data( const cooldown_t* cd )
  {
    for ( auto cdw : cooldown_waste_data_list )
    {
      if ( cdw->cd->name_str == cd->name_str )
        return cdw;
    }

    auto cdw = new cooldown_waste_data_t( cd );
    cooldown_waste_data_list.push_back( cdw );
    return cdw;
  }

  shatter_source_t* get_shatter_source( const std::string& name )
  {
    for ( auto ss : shatter_source_list )
    {
      if ( ss->name_str == name )
        return ss;
    }

    auto ss = new shatter_source_t( name );
    shatter_source_list.push_back( ss );
    return ss;
  }

  enum leyshock_trigger_e
  {
    LEYSHOCK_EXECUTE,
    LEYSHOCK_IMPACT,
    LEYSHOCK_TICK,
    LEYSHOCK_BUMP
  };

  void      update_rune_distance( double distance );
  action_t* get_icicle();
  bool      trigger_delayed_buff( buff_t* buff, double chance, timespan_t delay = 0.15_s );
  void      trigger_brain_freeze( double chance, proc_t* source );
  void      trigger_fof( double chance, int stacks, proc_t* source );
  void      trigger_icicle( player_t* icicle_target, bool chain = false );
  void      trigger_icicle_gain( player_t* icicle_target, action_t* icicle_action );
  void      trigger_evocation( timespan_t duration_override = timespan_t::min(), bool hasted = true );
  void      trigger_arcane_charge( int stacks = 1 );
  void      trigger_leyshock( unsigned id, const action_state_t* s, leyshock_trigger_e trigger_type );
  bool      trigger_crowd_control( const action_state_t* s, spell_mechanic type );
  void      trigger_lucid_dreams( player_t* trigger_target, double cost );

  void apl_precombat();
  void apl_arcane();
  void apl_fire();
  void apl_frost();
};

namespace pets {

struct mage_pet_t : public pet_t
{
  mage_pet_t( sim_t* sim, mage_t* owner, const std::string& pet_name,
              bool guardian = false, bool dynamic = false ) :
    pet_t( sim, owner, pet_name, guardian, dynamic )
  { }

  const mage_t* o() const
  { return static_cast<mage_t*>( owner ); }

  mage_t* o()
  { return static_cast<mage_t*>( owner ); }
};

struct mage_pet_spell_t : public spell_t
{
  mage_pet_spell_t( const std::string& n, mage_pet_t* p, const spell_data_t* s ) :
    spell_t( n, p, s )
  {
    may_crit = tick_may_crit = true;
    weapon_multiplier = 0.0;
  }

  mage_t* o()
  { return static_cast<mage_pet_t*>( player )->o(); }

  const mage_t* o() const
  { return static_cast<mage_pet_t*>( player )->o(); }
};

namespace water_elemental {

// ==========================================================================
// Pet Water Elemental
// ==========================================================================

struct water_elemental_pet_t : public mage_pet_t
{
  struct actions_t
  {
    action_t* freeze;
  } action;

  water_elemental_pet_t( sim_t* sim, mage_t* owner ) :
    mage_pet_t( sim, owner, "water_elemental" ),
    action()
  {
    owner_coeff.sp_from_sp = 0.75;
  }

  void init_action_list() override
  {
    action_list_str = "waterbolt";
    mage_pet_t::init_action_list();
  }

  action_t* create_action( const std::string&, const std::string& ) override;
  void      create_actions() override;
};

struct waterbolt_t : public mage_pet_spell_t
{
  waterbolt_t( const std::string& n, water_elemental_pet_t* p, const std::string& options_str ) :
    mage_pet_spell_t( n, p, p->find_pet_spell( "Waterbolt" ) )
  {
    parse_options( options_str );
    gcd_haste = HASTE_NONE;
  }
};

struct freeze_t : public mage_pet_spell_t
{
  freeze_t( const std::string& n, water_elemental_pet_t* p ) :
    mage_pet_spell_t( n, p, p->find_pet_spell( "Freeze" ) )
  {
    background = true;
    aoe = -1;
  }

  void impact( action_state_t* s ) override
  {
    mage_pet_spell_t::impact( s );
    o()->trigger_crowd_control( s, MECHANIC_ROOT );
  }
};

action_t* water_elemental_pet_t::create_action( const std::string& name, const std::string& options_str )
{
  if ( name == "waterbolt" ) return new waterbolt_t( name, this, options_str );

  return mage_pet_t::create_action( name, options_str );
}

void water_elemental_pet_t::create_actions()
{
  action.freeze = get_action<freeze_t>( "freeze", this );

  mage_pet_t::create_actions();
}

}  // water_elemental

namespace mirror_image {

// ==========================================================================
// Pet Mirror Image
// ==========================================================================

struct mirror_image_pet_t : public mage_pet_t
{
  buff_t* arcane_charge;

  mirror_image_pet_t( sim_t* sim, mage_t* owner ) :
    mage_pet_t( sim, owner, "mirror_image", true ),
    arcane_charge()
  {
    owner_coeff.sp_from_sp = 0.55;
  }

  action_t* create_action( const std::string&, const std::string& ) override;

  void init_action_list() override
  {
    switch ( o()->specialization() )
    {
      case MAGE_ARCANE:
        action_list_str = "arcane_blast";
        break;
      case MAGE_FIRE:
        action_list_str = "fireball";
        break;
      case MAGE_FROST:
        action_list_str = "frostbolt";
        break;
      default:
        break;
    }

    mage_pet_t::init_action_list();
  }

  void create_buffs() override
  {
    mage_pet_t::create_buffs();

    // MI Arcane Charge is hardcoded as 25% damage increase.
    arcane_charge = make_buff( this, "arcane_charge", o()->spec.arcane_charge )
                      ->set_default_value( 0.25 );
  }
};

struct mirror_image_spell_t : public mage_pet_spell_t
{
  mirror_image_spell_t( const std::string& n, mirror_image_pet_t* p, const spell_data_t* s ) :
    mage_pet_spell_t( n, p, s )
  { }

  void init_finished() override
  {
    stats = o()->pets.mirror_images.front()->get_stats( name_str );
    mage_pet_spell_t::init_finished();
  }

  mirror_image_pet_t* p() const
  {
    return static_cast<mirror_image_pet_t*>( player );
  }
};

struct arcane_blast_t : public mirror_image_spell_t
{
  arcane_blast_t( const std::string& n, mirror_image_pet_t* p, const std::string& options_str ) :
    mirror_image_spell_t( n, p, p->find_pet_spell( "Arcane Blast" ) )
  {
    parse_options( options_str );
  }

  void execute() override
  {
    mirror_image_spell_t::execute();
    p()->arcane_charge->trigger();
  }

  double action_multiplier() const override
  {
    double am = mirror_image_spell_t::action_multiplier();

    am *= 1.0 + p()->arcane_charge->check_stack_value();

    return am;
  }
};

struct fireball_t : public mirror_image_spell_t
{
  fireball_t( const std::string& n, mirror_image_pet_t* p, const std::string& options_str ) :
    mirror_image_spell_t( n, p, p->find_pet_spell( "Fireball" ) )
  {
    parse_options( options_str );
  }
};

struct frostbolt_t : public mirror_image_spell_t
{
  frostbolt_t( const std::string& n, mirror_image_pet_t* p, const std::string& options_str ) :
    mirror_image_spell_t( n, p, p->find_pet_spell( "Frostbolt" ) )
  {
    parse_options( options_str );
  }
};

action_t* mirror_image_pet_t::create_action( const std::string& name, const std::string& options_str )
{
  if ( name == "arcane_blast" ) return new arcane_blast_t( name, this, options_str );
  if ( name == "fireball"     ) return new     fireball_t( name, this, options_str );
  if ( name == "frostbolt"    ) return new    frostbolt_t( name, this, options_str );

  return mage_pet_t::create_action( name, options_str );
}

}  // mirror_image

}  // pets

namespace buffs {

// Touch of the Magi debuff =================================================

struct touch_of_the_magi_t : public buff_t
{
  double accumulated_damage;

  touch_of_the_magi_t( mage_td_t* td ) :
    buff_t( *td, "touch_of_the_magi", td->source->find_spell( 210824 ) ),
    accumulated_damage()
  {
    auto data = debug_cast<mage_t*>( source )->talents.touch_of_the_magi;
    set_chance( data->proc_chance() );
    set_cooldown( data->internal_cooldown() );
  }

  void reset() override
  {
    buff_t::reset();
    accumulated_damage = 0.0;
  }

  void expire_override( int stacks, timespan_t duration ) override
  {
    buff_t::expire_override( stacks, duration );

    auto p = debug_cast<mage_t*>( source );
    auto explosion = p->action.touch_of_the_magi;

    explosion->set_target( player );
    explosion->base_dd_min = explosion->base_dd_max =
      p->talents.touch_of_the_magi->effectN( 1 ).percent() * accumulated_damage;
    explosion->execute();

    accumulated_damage = 0.0;
  }

  void accumulate_damage( const action_state_t* s )
  {
    sim->print_debug(
      "{}'s {} accumulates {} additional damage: {} -> {}",
      player->name(), name(), s->result_total,
      accumulated_damage, accumulated_damage + s->result_total );

    accumulated_damage += s->result_total;
  }
};

// Custom buffs =============================================================

struct combustion_buff_t : public buff_t
{
  double current_amount;
  double multiplier;

  combustion_buff_t( mage_t* p ) :
    buff_t( p, "combustion", p->find_spell( 190319 ) ),
    current_amount(),
    multiplier( data().effectN( 3 ).percent() )
  {
    set_cooldown( 0_ms );
    set_default_value( data().effectN( 1 ).percent() );
    set_tick_zero( true );
    set_refresh_behavior( buff_refresh_behavior::DURATION );

    set_stack_change_callback( [ this ] ( buff_t*, int, int cur )
    {
      if ( cur == 0 )
      {
        player->stat_loss( STAT_MASTERY_RATING, current_amount );
        current_amount = 0.0;
      }
    } );

    set_tick_callback( [ this ] ( buff_t*, int, const timespan_t& )
    {
      double new_amount = multiplier * player->composite_spell_crit_rating();
      double diff = new_amount - current_amount;

      if ( diff > 0.0 ) player->stat_gain( STAT_MASTERY_RATING,  diff );
      if ( diff < 0.0 ) player->stat_loss( STAT_MASTERY_RATING, -diff );

      current_amount = new_amount;
    } );
  }

  void reset() override
  {
    buff_t::reset();
    current_amount = 0.0;
  }
};

struct ice_floes_buff_t : public buff_t
{
  ice_floes_buff_t( mage_t* p ) :
    buff_t( p, "ice_floes", p->talents.ice_floes )
  { }

  void decrement( int stacks, double value ) override
  {
    if ( check() == 0 )
      return;

    if ( sim->current_time() - last_trigger > 0.5_s )
      buff_t::decrement( stacks, value );
    else
      sim->print_debug( "Ice Floes removal ignored due to 500 ms protection" );
  }
};

struct icy_veins_buff_t : public buff_t
{
  icy_veins_buff_t( mage_t* p ) :
    buff_t( p, "icy_veins", p->find_spell( 12472 ) )
  {
    set_default_value( data().effectN( 1 ).percent() );
    set_cooldown( 0_ms );
    add_invalidate( CACHE_SPELL_HASTE );
    buff_duration += p->talents.thermal_void->effectN( 2 ).time_value();
  }

  void expire_override( int stacks, timespan_t duration ) override
  {
    buff_t::expire_override( stacks, duration );

    auto mage = debug_cast<mage_t*>( player );
    if ( mage->talents.thermal_void->ok() && duration == 0_ms )
      mage->sample_data.icy_veins_duration->add( elapsed( sim->current_time() ).total_seconds() );

    mage->buffs.frigid_grasp->expire();
  }
};

struct incanters_flow_t : public buff_t
{
  incanters_flow_t( mage_t* p ) :
    buff_t( p, "incanters_flow", p->find_spell( 116267 ) )
  {
    set_duration( 0_ms );
    set_period( p->talents.incanters_flow->effectN( 1 ).period() );
    set_chance( p->talents.incanters_flow->ok() );
    set_default_value( data().effectN( 1 ).percent() );

    // Leyshock
    set_stack_change_callback( [ p ] ( buff_t* b, int old, int cur )
    {
      if ( old == 3 && cur == 4 )
        p->trigger_leyshock( b->data().id(), nullptr, mage_t::LEYSHOCK_BUMP );
    } );
  }

  void reset() override
  {
    buff_t::reset();
    reverse = false;
  }

  void bump( int stacks, double value ) override
  {
    if ( check() == max_stack() )
      reverse = true;
    else
      buff_t::bump( stacks, value );
  }

  void decrement( int stacks, double value ) override
  {
    if ( check() == 1 )
      reverse = false;
    else
      buff_t::decrement( stacks, value );
  }
};

}  // buffs


namespace actions {

// ==========================================================================
// Mage Spell
// ==========================================================================

struct mage_spell_state_t : public action_state_t
{
  // Simple bitfield for tracking sources of the Frozen effect.
  unsigned frozen;

  // Damage multiplier that is in efffect only for frozen targets.
  double frozen_multiplier;

  mage_spell_state_t( action_t* action, player_t* target ) :
    action_state_t( action, target ),
    frozen(),
    frozen_multiplier( 1.0 )
  { }

  void initialize() override
  {
    action_state_t::initialize();
    frozen = 0u;
    frozen_multiplier = 1.0;
  }

  std::ostringstream& debug_str( std::ostringstream& s ) override
  {
    action_state_t::debug_str( s ) << " frozen=";

    std::streamsize ss = s.precision();
    s.precision( 4 );

    if ( frozen )
    {
      std::string str;

      auto concat_flag_str = [ this, &str ] ( const char* flag_str, frozen_flag_e flag )
      {
        if ( frozen & flag )
        {
          if ( !str.empty() )
            str += "|";
          str += flag_str;
        }
      };

      concat_flag_str( "WC", FF_WINTERS_CHILL );
      concat_flag_str( "FOF", FF_FINGERS_OF_FROST );
      concat_flag_str( "ROOT", FF_ROOT );

      s << "{ " << str << " }";
    }
    else
    {
      s << "0";
    }

    s << " frozen_mul=" << frozen_multiplier;

    s.precision( ss );

    return s;
  }

  void copy_state( const action_state_t* s ) override
  {
    action_state_t::copy_state( s );

    auto mss = debug_cast<const mage_spell_state_t*>( s );
    frozen            = mss->frozen;
    frozen_multiplier = mss->frozen_multiplier;
  }

  double composite_crit_chance() const override;

  double composite_da_multiplier() const override
  { return action_state_t::composite_da_multiplier() * frozen_multiplier; }

  double composite_ta_multiplier() const override
  { return action_state_t::composite_ta_multiplier() * frozen_multiplier; }
};

struct mage_spell_t : public spell_t
{
  struct affected_by_t
  {
    // Permanent damage increase.
    bool arcane_mage = true;
    bool fire_mage = true;
    bool frost_mage = true;

    // Temporary damage increase.
    bool arcane_power = true;
    bool bone_chilling = true;
    bool crackling_energy = true;
    bool incanters_flow = true;
    bool rune_of_power = true;

    // Misc
    bool combustion = true;
    bool ice_floes = false;
    bool shatter = false;
  } affected_by;

  static const snapshot_state_e STATE_FROZEN     = STATE_TGT_USER_1;
  static const snapshot_state_e STATE_FROZEN_MUL = STATE_TGT_USER_2;

  bool track_cd_waste;
  cooldown_waste_data_t* cd_waste;

public:
  mage_spell_t( const std::string& n, mage_t* p, const spell_data_t* s = spell_data_t::nil() ) :
    spell_t( n, p, s ),
    affected_by(),
    track_cd_waste(),
    cd_waste()
  {
    may_crit = tick_may_crit = true;
    weapon_multiplier = 0.0;
    affected_by.ice_floes = data().affected_by( p->talents.ice_floes->effectN( 1 ) );
    track_cd_waste = data().cooldown() > 0_ms || data().charge_cooldown() > 0_ms;
  }

  mage_t* p()
  { return static_cast<mage_t*>( player ); }

  const mage_t* p() const
  { return static_cast<mage_t*>( player ); }

  mage_spell_state_t* cast_state( action_state_t* s )
  { return debug_cast<mage_spell_state_t*>( s ); }

  const mage_spell_state_t* cast_state( const action_state_t* s ) const
  { return debug_cast<const mage_spell_state_t*>( s ); }

  mage_td_t* td( player_t* t ) const
  { return p()->get_target_data( t ); }

  action_state_t* new_state() override
  { return new mage_spell_state_t( this, target ); }

  void init() override
  {
    if ( initialized )
      return;

    spell_t::init();

    if ( affected_by.arcane_mage )
      base_multiplier *= 1.0 + p()->spec.arcane_mage->effectN( 1 ).percent();

    if ( affected_by.fire_mage )
      base_multiplier *= 1.0 + p()->spec.fire_mage->effectN( 1 ).percent();

    if ( affected_by.frost_mage )
      base_multiplier *= 1.0 + p()->spec.frost_mage->effectN( 1 ).percent();

    if ( harmful && affected_by.shatter )
    {
      snapshot_flags |= STATE_FROZEN | STATE_FROZEN_MUL;
      update_flags   |= STATE_FROZEN | STATE_FROZEN_MUL;
    }
  }

  void init_finished() override
  {
    if ( track_cd_waste && sim->report_details != 0 )
      cd_waste = p()->get_cooldown_waste_data( cooldown );

    spell_t::init_finished();
  }

  double action_multiplier() const override
  {
    double m = spell_t::action_multiplier();

    if ( affected_by.arcane_power )
      m *= 1.0 + p()->buffs.arcane_power->check_value();

    if ( affected_by.bone_chilling )
      m *= 1.0 + p()->buffs.bone_chilling->check_stack_value();

    if ( affected_by.incanters_flow )
      m *= 1.0 + p()->buffs.incanters_flow->check_stack_value();

    if ( affected_by.rune_of_power )
      m *= 1.0 + p()->buffs.rune_of_power->check_value();

    return m;
  }

  double composite_crit_chance() const override
  {
    double c = spell_t::composite_crit_chance();

    if ( affected_by.combustion )
      c += p()->buffs.combustion->check_value();

    return c;
  }

  virtual unsigned frozen( const action_state_t* s ) const
  {
    const mage_td_t* td = p()->target_data[ s->target ];

    if ( !td )
      return 0u;

    unsigned source = 0u;

    if ( td->debuffs.winters_chill->check() )
      source |= FF_WINTERS_CHILL;

    if ( td->debuffs.frozen->check() )
      source |= FF_ROOT;

    return source;
  }

  virtual double frozen_multiplier( const action_state_t* ) const
  { return 1.0; }

  void snapshot_internal( action_state_t* s, unsigned flags, dmg_e rt ) override
  {
    spell_t::snapshot_internal( s, flags, rt );

    if ( flags & STATE_FROZEN )
      cast_state( s )->frozen = frozen( s );

    if ( flags & STATE_FROZEN_MUL )
      cast_state( s )->frozen_multiplier = cast_state( s )->frozen ? frozen_multiplier( s ) : 1.0;
  }

  double cost() const override
  {
    double c = spell_t::cost();

    if ( p()->buffs.arcane_power->check() )
    {
      c *= 1.0 + p()->buffs.arcane_power->data().effectN( 2 ).percent()
               + p()->talents.overpowered->effectN( 2 ).percent();
    }

    return c;
  }

  void update_ready( timespan_t cd ) override
  {
    if ( cd_waste )
      cd_waste->add( cd, time_to_execute );

    spell_t::update_ready( cd );
  }

  bool usable_moving() const override
  {
    if ( p()->buffs.ice_floes->check() && affected_by.ice_floes )
      return true;

    return spell_t::usable_moving();
  }

  virtual void consume_cost_reductions()
  { }

  void execute() override
  {
    spell_t::execute();
    p()->trigger_leyshock( id, execute_state, mage_t::LEYSHOCK_EXECUTE );

    // Make sure we remove all cost reduction buffs before we trigger new ones.
    // This will prevent for example Arcane Blast consuming its own Clearcasting proc.
    consume_cost_reductions();

    if ( p()->spec.clearcasting->ok() && harmful && current_resource() == RESOURCE_MANA )
    {
      // Mana spending required for 1% chance.
      double mana_step = p()->spec.clearcasting->cost( POWER_MANA ) * p()->resources.base[ RESOURCE_MANA ];
      mana_step /= p()->spec.clearcasting->effectN( 1 ).percent();

      double proc_chance = 0.01 * last_resource_cost / mana_step;
      proc_chance *= 1.0 + p()->azerite.arcane_pummeling.spell_ref().effectN( 2 ).percent();
      p()->trigger_delayed_buff( p()->buffs.clearcasting, proc_chance );
    }

    if ( !background && affected_by.ice_floes && time_to_execute > 0_ms )
      p()->buffs.ice_floes->decrement();
  }

  void tick( dot_t* d ) override
  {
    spell_t::tick( d );
    p()->trigger_leyshock( id, d->state, mage_t::LEYSHOCK_TICK );
  }

  void last_tick( dot_t* d ) override
  {
    spell_t::last_tick( d );

    if ( channeled && affected_by.ice_floes )
      p()->buffs.ice_floes->decrement();
  }

  void impact( action_state_t* s ) override
  {
    spell_t::impact( s );
    p()->trigger_leyshock( id, s, mage_t::LEYSHOCK_IMPACT );
  }

  void consume_resource() override
  {
    spell_t::consume_resource();

    if ( current_resource() == RESOURCE_MANA )
      p()->trigger_lucid_dreams( target, last_resource_cost );
  }
};

typedef residual_action::residual_periodic_action_t<mage_spell_t> residual_action_t;

double mage_spell_state_t::composite_crit_chance() const
{
  double c = action_state_t::composite_crit_chance();

  if ( frozen )
  {
    auto a = debug_cast<const mage_spell_t*>( action );
    auto p = a->p();

    if ( a->affected_by.shatter && p->spec.shatter->ok() )
    {
      // Multiplier is not in spell data, apparently.
      c *= 1.5;
      c += p->spec.shatter->effectN( 2 ).percent() + p->spec.shatter_2->effectN( 1 ).percent();
    }
  }

  return c;
}


// ==========================================================================
// Arcane Mage Spell
// ==========================================================================

struct arcane_mage_spell_t : public mage_spell_t
{
  std::vector<buff_t*> cost_reductions;

  arcane_mage_spell_t( const std::string& n, mage_t* p, const spell_data_t* s = spell_data_t::nil() ) :
    mage_spell_t( n, p, s ),
    cost_reductions()
  { }

  void consume_cost_reductions() override
  {
    // Consume first applicable buff and then stop.
    for ( auto cr : cost_reductions )
    {
      if ( cr->check() )
      {
        cr->decrement();
        break;
      }
    }
  }

  double cost() const override
  {
    double c = mage_spell_t::cost();

    for ( auto cr : cost_reductions )
      c *= 1.0 + cr->check_value();

    return std::max( c, 0.0 );
  }

  double arcane_charge_multiplier( bool arcane_barrage = false ) const
  {
    double per_ac_bonus = 0.0;

    if ( arcane_barrage )
    {
      per_ac_bonus = p()->spec.arcane_charge->effectN( 2 ).percent()
                   + p()->cache.mastery() * p()->spec.savant->effectN( 3 ).mastery_value();
    }
    else
    {
      per_ac_bonus = p()->spec.arcane_charge->effectN( 1 ).percent()
                   + p()->cache.mastery() * p()->spec.savant->effectN( 2 ).mastery_value();
    }

    return 1.0 + p()->buffs.arcane_charge->check() * per_ac_bonus;
  }
};


// ==========================================================================
// Fire Mage Spell
// ==========================================================================

struct fire_mage_spell_t : public mage_spell_t
{
  bool triggers_hot_streak;
  bool triggers_ignite;
  bool triggers_kindling;

  fire_mage_spell_t( const std::string& n, mage_t* p, const spell_data_t* s = spell_data_t::nil() ) :
    mage_spell_t( n, p, s ),
    triggers_hot_streak(),
    triggers_ignite(),
    triggers_kindling()
  { }

  void impact( action_state_t* s ) override
  {
    mage_spell_t::impact( s );

    if ( result_is_hit( s->result ) )
    {
      if ( triggers_ignite )
        trigger_ignite( s );

      if ( triggers_hot_streak )
        handle_hot_streak( s );

      if ( triggers_kindling && p()->talents.kindling->ok() && s->result == RESULT_CRIT )
        p()->cooldowns.combustion->adjust( -p()->talents.kindling->effectN( 1 ).time_value() );
    }
  }

  void handle_hot_streak( action_state_t* s )
  {
    mage_t* p = this->p();
    if ( !p->spec.hot_streak->ok() )
      return;

    bool guaranteed = s->composite_crit_chance() >= 1.0;
    p->procs.hot_streak_spell->occur();

    if ( s->result == RESULT_CRIT )
    {
      p->procs.hot_streak_spell_crit->occur();

      // Crit with HS => wasted crit
      if ( p->buffs.hot_streak->check() )
      {
        p->procs.hot_streak_spell_crit_wasted->occur();
        if ( guaranteed )
          p->buffs.hot_streak->predict();
      }
      else
      {
        // Crit with HU => convert to HS
        if ( p->buffs.heating_up->up() )
        {
          p->procs.hot_streak->occur();
          // Check if HS was triggered by IB
          if ( id == 108853 )
            p->procs.heating_up_ib_converted->occur();

          bool hu_react = p->buffs.heating_up->stack_react() > 0;
          p->buffs.heating_up->expire();
          p->buffs.hot_streak->trigger();
          if ( guaranteed && hu_react )
            p->buffs.hot_streak->predict();

          // If Scorch generates Hot Streak and the actor is currently casting Pyroblast,
          // the game will immediately finish the cast. This is presumably done to work
          // around the buff application delay inside Combustion or with Searing Touch
          // active. The following code is a huge hack.
          if ( id == 2948 && p->executing && p->executing->id == 11366 )
          {
            assert( p->executing->execute_event );
            event_t::cancel( p->executing->execute_event );
            event_t::cancel( p->cast_while_casting_poll_event );
            // We need to set time_to_execute to zero, start a new action execute event and
            // adjust GCD. action_t::schedule_execute should handle all these.
            p->executing->schedule_execute();
          }
        }
        // Crit without HU => generate HU
        else
        {
          p->procs.heating_up_generated->occur();
          p->buffs.heating_up->trigger(
            1, buff_t::DEFAULT_VALUE(), -1.0,
            p->buffs.heating_up->buff_duration * p->cache.spell_speed() );
          if ( guaranteed )
            p->buffs.heating_up->predict();
        }
      }
    }
    else // Non-crit
    {
      // Non-crit with HU => remove HU
      if ( p->buffs.heating_up->check() )
      {
        if ( p->buffs.heating_up->elapsed( sim->current_time() ) > 0.2_s )
        {
          p->procs.heating_up_removed->occur();
          p->buffs.heating_up->expire();
          sim->print_debug( "Heating Up removed by non-crit" );
        }
        else
        {
          sim->print_debug( "Heating Up removal ignored due to 200 ms protection" );
        }
      }
    }
  }

  virtual double composite_ignite_multiplier( const action_state_t* ) const
  { return 1.0; }

  void trigger_ignite( action_state_t* s )
  {
    if ( !p()->spec.ignite->ok() )
      return;

    double m = s->target_da_multiplier;
    if ( m <= 0.0 )
      return;

    double amount = s->result_total / m * p()->cache.mastery_value();
    if ( amount <= 0.0 )
      return;

    amount *= composite_ignite_multiplier( s );

    if ( !p()->ignite->get_dot( s->target )->is_ticking() )
      p()->procs.ignite_applied->occur();

    residual_action::trigger( p()->ignite, s->target, amount );

    if ( s->chain_target > 0 )
      return;

    auto& bm = p()->benefits.blaster_master;
    if ( bm.combustion && p()->buffs.combustion->check() )
      bm.combustion->update();
    if ( bm.rune_of_power && p()->buffs.rune_of_power->check() )
      bm.rune_of_power->update();
    if ( bm.searing_touch && s->target->health_percentage() < p()->talents.searing_touch->effectN( 1 ).base_value() )
      bm.searing_touch->update();
  }

  bool firestarter_active( player_t* target ) const
  {
    if ( !p()->talents.firestarter->ok() )
      return false;

    // Check for user-specified override.
    if ( p()->options.firestarter_time > 0_ms )
      return sim->current_time() < p()->options.firestarter_time;
    else
      return target->health_percentage() > p()->talents.firestarter->effectN( 1 ).base_value();
  }
};

struct hot_streak_state_t : public mage_spell_state_t
{
  bool hot_streak;

  hot_streak_state_t( action_t* action, player_t* target ) :
    mage_spell_state_t( action, target ),
    hot_streak()
  { }

  void initialize() override
  {
    mage_spell_state_t::initialize();
    hot_streak = false;
  }

  std::ostringstream& debug_str( std::ostringstream& s ) override
  {
    mage_spell_state_t::debug_str( s ) << " hot_streak=" << hot_streak;
    return s;
  }

  void copy_state( const action_state_t* s ) override
  {
    mage_spell_state_t::copy_state( s );
    hot_streak = debug_cast<const hot_streak_state_t*>( s )->hot_streak;
  }
};

struct hot_streak_spell_t : public fire_mage_spell_t
{
  // Last available Hot Streak state.
  bool last_hot_streak;

  hot_streak_spell_t( const std::string& n, mage_t* p, const spell_data_t* s = spell_data_t::nil() ) :
    fire_mage_spell_t( n, p, s ),
    last_hot_streak()
  { }

  action_state_t* new_state() override
  { return new hot_streak_state_t( this, target ); }

  timespan_t execute_time() const override
  {
    if ( p()->buffs.hot_streak->check() )
      return 0_ms;

    return fire_mage_spell_t::execute_time();
  }

  void snapshot_state( action_state_t* s, dmg_e rt ) override
  {
    fire_mage_spell_t::snapshot_state( s, rt );
    debug_cast<hot_streak_state_t*>( s )->hot_streak = last_hot_streak;
  }

  double composite_ignite_multiplier( const action_state_t* s ) const override
  {
    return debug_cast<const hot_streak_state_t*>( s )->hot_streak ? 2.0 : 1.0;
  }

  void execute() override
  {
    last_hot_streak = p()->buffs.hot_streak->up() && time_to_execute == 0_ms;
    fire_mage_spell_t::execute();

    if ( last_hot_streak )
    {
      p()->buffs.hot_streak->expire();
      p()->buffs.pyroclasm->trigger();
      p()->buffs.firemind->trigger();

      if ( rng().roll( p()->talents.pyromaniac->effectN( 1 ).percent() ) )
      {
        p()->procs.hot_streak->occur();
        p()->procs.hot_streak_pyromaniac->occur();
        p()->buffs.hot_streak->trigger();
      }
    }
  }
};


// ==========================================================================
// Frost Mage Spell
// ==========================================================================

// Some Frost spells snapshot on impact (rather than execute). This is handled via
// the calculate_on_impact flag.
//
// When set to true:
//   * All snapshot flags are moved from snapshot_flags to impact_flags.
//   * calculate_result and calculate_direct_amount don't do any calculations.
//   * On spell impact:
//     - State is snapshot via frost_mage_spell_t::snapshot_impact_state.
//     - Result is calculated via frost_mage_spell_t::calculate_impact_result.
//     - Amount is calculated via frost_mage_spell_t::calculate_impact_direct_amount.
//
// The previous functions are virtual and can be overridden when needed.
struct frost_mage_spell_t : public mage_spell_t
{
  bool chills;
  bool calculate_on_impact;

  proc_t* proc_brain_freeze;
  proc_t* proc_fof;

  bool track_shatter;
  shatter_source_t* shatter_source;

  unsigned impact_flags;

  frost_mage_spell_t( const std::string& n, mage_t* p, const spell_data_t* s = spell_data_t::nil() ) :
    mage_spell_t( n, p, s ),
    chills(),
    calculate_on_impact(),
    proc_brain_freeze(),
    proc_fof(),
    track_shatter(),
    shatter_source(),
    impact_flags()
  {
    affected_by.shatter = true;
  }

  void init() override
  {
    if ( initialized )
      return;

    mage_spell_t::init();

    if ( calculate_on_impact )
      std::swap( snapshot_flags, impact_flags );
  }

  void init_finished() override
  {
    mage_spell_t::init_finished();

    if ( track_shatter && sim->report_details != 0 )
      shatter_source = p()->get_shatter_source( name_str );
  }

  double icicle_sp_coefficient() const
  {
    return p()->cache.mastery() * p()->spec.icicles->effectN( 3 ).sp_coeff();
  }

  virtual void snapshot_impact_state( action_state_t* s, dmg_e rt )
  { snapshot_internal( s, impact_flags, rt ); }

  double calculate_direct_amount( action_state_t* s ) const override
  {
    if ( !calculate_on_impact )
    {
      return mage_spell_t::calculate_direct_amount( s );
    }
    else
    {
      // Don't do any extra work, this result won't be used.
      return 0.0;
    }
  }

  virtual double calculate_impact_direct_amount( action_state_t* s ) const
  { return mage_spell_t::calculate_direct_amount( s ); }

  result_e calculate_result( action_state_t* s ) const override
  {
    if ( !calculate_on_impact )
    {
      return mage_spell_t::calculate_result( s );
    }
    else
    {
      // Don't do any extra work, this result won't be used.
      return RESULT_NONE;
    }
  }

  virtual result_e calculate_impact_result( action_state_t* s ) const
  { return mage_spell_t::calculate_result( s ); }

  void record_shatter_source( const action_state_t* s, shatter_source_t* source )
  {
    if ( !source )
      return;

    unsigned frozen = cast_state( s )->frozen;

    if ( frozen & FF_WINTERS_CHILL )
      source->occur( FROZEN_WINTERS_CHILL );
    else if ( frozen & ~FF_FINGERS_OF_FROST )
      source->occur( FROZEN_ROOT );
    else if ( frozen & FF_FINGERS_OF_FROST )
      source->occur( FROZEN_FINGERS_OF_FROST );
    else
      source->occur( FROZEN_NONE );
  }

  void impact( action_state_t* s ) override
  {
    if ( calculate_on_impact )
    {
      // Spells that calculate damage on impact need to snapshot relevant values
      // right before impact and then recalculate the result and total damage.
      snapshot_impact_state( s, amount_type( s ) );
      s->result = calculate_impact_result( s );
      s->result_amount = calculate_impact_direct_amount( s );
    }

    mage_spell_t::impact( s );

    if ( result_is_hit( s->result ) && s->chain_target == 0 )
      record_shatter_source( s, shatter_source );

    if ( result_is_hit( s->result ) && chills )
      p()->buffs.bone_chilling->trigger();
  }
};


// Icicles ==================================================================

struct icicle_t : public frost_mage_spell_t
{
  icicle_t( const std::string& n, mage_t* p ) :
    frost_mage_spell_t( n, p, p->find_spell( 148022 ) )
  {
    background = true;
    callbacks = false;
    base_dd_min = base_dd_max = 1.0;
    base_dd_adder += p->azerite.flash_freeze.value( 2 );

    if ( p->talents.splitting_ice->ok() )
    {
      aoe = as<int>( 1 + p->talents.splitting_ice->effectN( 1 ).base_value() );
      base_multiplier *= 1.0 + p->talents.splitting_ice->effectN( 3 ).percent();
      base_aoe_multiplier *= p->talents.splitting_ice->effectN( 2 ).percent();
    }
  }

  void init_finished() override
  {
    proc_fof = p()->get_proc( "Fingers of Frost from Flash Freeze" );
    frost_mage_spell_t::init_finished();
  }

  void execute() override
  {
    frost_mage_spell_t::execute();
    p()->buffs.icicles->decrement();
  }

  void impact( action_state_t* s ) override
  {
    frost_mage_spell_t::impact( s );

    if ( result_is_hit( s->result ) )
      p()->trigger_fof( p()->azerite.flash_freeze.spell_ref().effectN( 1 ).percent(), 1, proc_fof );
  }

  double spell_direct_power_coefficient( const action_state_t* s ) const override
  { return frost_mage_spell_t::spell_direct_power_coefficient( s ) + icicle_sp_coefficient(); }
};

// Presence of Mind Spell ===================================================

struct presence_of_mind_t : public arcane_mage_spell_t
{
  presence_of_mind_t( const std::string& n, mage_t* p, const std::string& options_str ) :
    arcane_mage_spell_t( n, p, p->find_specialization_spell( "Presence of Mind" ) )
  {
    parse_options( options_str );
    harmful = false;
  }

  bool ready() override
  {
    if ( p()->buffs.presence_of_mind->check() )
      return false;

    return arcane_mage_spell_t::ready();
  }

  void execute() override
  {
    arcane_mage_spell_t::execute();
    p()->buffs.presence_of_mind->trigger( p()->buffs.presence_of_mind->max_stack() );
  }
};

// Ignite Spell =============================================================

struct ignite_t : public residual_action_t
{
  ignite_t( const std::string& n, mage_t* p ) :
    residual_action_t( n, p, p->find_spell( 12654 ) )
  {
    callbacks = true;
  }

  void init() override
  {
    residual_action_t::init();

    snapshot_flags |= STATE_TGT_MUL_TA;
    update_flags   |= STATE_TGT_MUL_TA;
  }

  void tick( dot_t* d ) override
  {
    residual_action_t::tick( d );

    if ( rng().roll( p()->talents.conflagration->effectN( 1 ).percent() ) )
    {
      p()->action.conflagration_flare_up->set_target( d->target );
      p()->action.conflagration_flare_up->execute();
    }
  }
};

// Arcane Barrage Spell =====================================================

struct arcane_barrage_t : public arcane_mage_spell_t
{
  arcane_barrage_t( const std::string& n, mage_t* p, const std::string& options_str ) :
    arcane_mage_spell_t( n, p, p->find_specialization_spell( "Arcane Barrage" ) )
  {
    parse_options( options_str );
    cooldown->hasted = true;
    base_aoe_multiplier *= data().effectN( 2 ).percent();
  }

  int n_targets() const override
  {
    int charges = p()->buffs.arcane_charge->check();
    return p()->spec.arcane_barrage_2->ok() && charges > 0 ? charges + 1 : 0;
  }

  void execute() override
  {
    p()->benefits.arcane_charge.arcane_barrage->update();

    arcane_mage_spell_t::execute();

    p()->buffs.arcane_charge->expire();
  }

  void impact( action_state_t* s ) override
  {
    arcane_mage_spell_t::impact( s );

    if ( result_is_hit( s->result ) )
      p()->buffs.chrono_shift->trigger();
  }

  double bonus_da( const action_state_t* s ) const override
  {
    double da = arcane_mage_spell_t::bonus_da( s );

    if ( s->target->health_percentage() < p()->azerite.arcane_pressure.spell_ref().effectN( 2 ).base_value() )
      da += p()->azerite.arcane_pressure.value() * p()->buffs.arcane_charge->check() / arcane_charge_multiplier( true );

    return da;
  }

  double composite_da_multiplier( const action_state_t* s ) const override
  {
    double m = arcane_mage_spell_t::composite_da_multiplier( s );

    m *= 1.0 + s->n_targets * p()->talents.resonance->effectN( 1 ).percent();

    return m;
  }

  double action_multiplier() const override
  {
    double am = arcane_mage_spell_t::action_multiplier();

    am *= arcane_charge_multiplier( true );

    return am;
  }
};

// Arcane Blast Spell =======================================================

struct arcane_blast_t : public arcane_mage_spell_t
{
  double equipoise_threshold;
  double equipoise_reduction;

  arcane_blast_t( const std::string& n, mage_t* p, const std::string& options_str ) :
    arcane_mage_spell_t( n, p, p->find_specialization_spell( "Arcane Blast" ) ),
    equipoise_threshold(),
    equipoise_reduction()
  {
    parse_options( options_str );
    cost_reductions = { p->buffs.rule_of_threes };
    base_dd_adder += p->azerite.galvanizing_spark.value( 2 );

    if ( p->azerite.equipoise.enabled() )
    {
      // Equipoise data is stored across 4 different spells.
      equipoise_threshold = p->find_spell( 264351 )->effectN( 1 ).percent();
      equipoise_reduction = p->find_spell( 264353 )->effectN( 1 ).average( p );
    }
  }

  double cost() const override
  {
    double c = arcane_mage_spell_t::cost();

    // TODO: It looks like the flat cost reduction is applied after Arcane Power et al,
    // but before Arcane Charge. This might not be intended, double check later.
    if ( p()->resources.pct( RESOURCE_MANA ) <= equipoise_threshold )
      c += equipoise_reduction;

    c *= 1.0 + p()->buffs.arcane_charge->check()
             * p()->spec.arcane_charge->effectN( 5 ).percent();

    return std::max( c, 0.0 );
  }

  double bonus_da( const action_state_t* s ) const override
  {
    double da = arcane_mage_spell_t::bonus_da( s );

    if ( p()->resources.pct( RESOURCE_MANA ) > equipoise_threshold )
      da += p()->azerite.equipoise.value();

    return da;
  }

  void execute() override
  {
    p()->benefits.arcane_charge.arcane_blast->update();
    arcane_mage_spell_t::execute();

    if ( hit_any_target )
    {
      p()->trigger_arcane_charge();

      // TODO: Benefit tracking
      if ( rng().roll( p()->azerite.galvanizing_spark.spell_ref().effectN( 1 ).percent() ) )
        p()->trigger_arcane_charge();
    }

    if ( p()->buffs.presence_of_mind->up() )
      p()->buffs.presence_of_mind->decrement();
  }

  double action_multiplier() const override
  {
    double am = arcane_mage_spell_t::action_multiplier();

    am *= arcane_charge_multiplier();

    return am;
  }

  timespan_t execute_time() const override
  {
    if ( p()->buffs.presence_of_mind->check() )
      return 0_ms;

    timespan_t t = arcane_mage_spell_t::execute_time();

    t *= 1.0 + p()->buffs.arcane_charge->check()
             * p()->spec.arcane_charge->effectN( 4 ).percent();

    return t;
  }

  void impact( action_state_t* s ) override
  {
    arcane_mage_spell_t::impact( s );

    if ( result_is_hit( s->result ) )
      td( s->target )->debuffs.touch_of_the_magi->trigger();
  }
};

// Arcane Explosion Spell ===================================================

struct arcane_explosion_t : public arcane_mage_spell_t
{
  arcane_explosion_t( const std::string& n, mage_t* p, const std::string& options_str ) :
    arcane_mage_spell_t( n, p, p->find_specialization_spell( "Arcane Explosion" ) )
  {
    parse_options( options_str );
    aoe = -1;
    cost_reductions = { p->buffs.clearcasting };
    base_dd_adder += p->azerite.explosive_echo.value( 2 );
  }

  void execute() override
  {
    arcane_mage_spell_t::execute();

    if ( hit_any_target )
      p()->trigger_arcane_charge();

    if ( num_targets_hit >= as<int>( p()->talents.reverberate->effectN( 2 ).base_value() )
      && rng().roll( p()->talents.reverberate->effectN( 1 ).percent() ) )
    {
      p()->trigger_arcane_charge();
    }
  }

  double bonus_da( const action_state_t* s ) const override
  {
    double da = arcane_mage_spell_t::bonus_da( s );

    if ( target_list().size() >= as<size_t>( p()->azerite.explosive_echo.spell_ref().effectN( 1 ).base_value() )
      && rng().roll( p()->azerite.explosive_echo.spell_ref().effectN( 3 ).percent() ) )
    {
      da += p()->azerite.explosive_echo.value( 4 );
    }

    return da;
  }
};

// Arcane Familiar Spell ====================================================

struct arcane_assault_t : public arcane_mage_spell_t
{
  arcane_assault_t( const std::string& n, mage_t* p ) :
    arcane_mage_spell_t( n, p, p->find_spell( 225119 ) )
  {
    background = true;
  }
};

struct arcane_familiar_t : public arcane_mage_spell_t
{
  arcane_familiar_t( const std::string& n, mage_t* p, const std::string& options_str ) :
    arcane_mage_spell_t( n, p, p->talents.arcane_familiar )
  {
    parse_options( options_str );
    harmful = track_cd_waste = false;
    ignore_false_positive = true;
  }

  void execute() override
  {
    arcane_mage_spell_t::execute();
    p()->buffs.arcane_familiar->trigger();
  }

  bool ready() override
  {
    if ( p()->buffs.arcane_familiar->check() )
      return false;

    return arcane_mage_spell_t::ready();
  }
};

// Arcane Intellect Spell ===================================================

struct arcane_intellect_t : public mage_spell_t
{
  arcane_intellect_t( const std::string& n, mage_t* p, const std::string& options_str ) :
    mage_spell_t( n, p, p->find_class_spell( "Arcane Intellect" ) )
  {
    parse_options( options_str );
    harmful = false;
    ignore_false_positive = true;
    background = sim->overrides.arcane_intellect != 0;
  }

  void execute() override
  {
    mage_spell_t::execute();

    if ( !sim->overrides.arcane_intellect )
      sim->auras.arcane_intellect->trigger();
  }
};

// Arcane Missiles Spell ====================================================

struct arcane_missiles_tick_t : public arcane_mage_spell_t
{
  arcane_missiles_tick_t( const std::string& n, mage_t* p ) :
    arcane_mage_spell_t( n, p, p->find_spell( 7268 ) )
  {
    background = true;
  }

  void execute() override
  {
    arcane_mage_spell_t::execute();

    if ( p()->buffs.clearcasting_channel->check() )
      p()->buffs.arcane_pummeling->trigger();
  }

  double bonus_da( const action_state_t* s ) const override
  {
    double da = arcane_mage_spell_t::bonus_da( s );

    da += p()->buffs.arcane_pummeling->check_stack_value();

    return da;
  }
};

struct am_state_t : public mage_spell_state_t
{
  double tick_time_multiplier;

  am_state_t( action_t* action, player_t* target ) :
    mage_spell_state_t( action, target ),
    tick_time_multiplier( 1.0 )
  { }

  void initialize() override
  {
    mage_spell_state_t::initialize();
    tick_time_multiplier = 1.0;
  }

  std::ostringstream& debug_str( std::ostringstream& s ) override
  {
    mage_spell_state_t::debug_str( s ) << " tick_time_multiplier=" << tick_time_multiplier;
    return s;
  }

  void copy_state( const action_state_t* s ) override
  {
    mage_spell_state_t::copy_state( s );
    tick_time_multiplier = debug_cast<const am_state_t*>( s )->tick_time_multiplier;
  }
};

struct arcane_missiles_t : public arcane_mage_spell_t
{
  double cc_duration_reduction;
  double cc_tick_time_reduction;

  arcane_missiles_t( const std::string& n, mage_t* p, const std::string& options_str ) :
    arcane_mage_spell_t( n, p, p->find_specialization_spell( "Arcane Missiles" ) )
  {
    parse_options( options_str );
    may_miss = false;
    tick_zero = channeled = true;
    tick_action = get_action<arcane_missiles_tick_t>( "arcane_missiles_tick", p );
    cost_reductions = { p->buffs.clearcasting, p->buffs.rule_of_threes };

    auto cc_data = p->buffs.clearcasting_channel->data();
    cc_duration_reduction  = cc_data.effectN( 1 ).percent();
    cc_tick_time_reduction = cc_data.effectN( 2 ).percent() + p->talents.amplification->effectN( 1 ).percent();
  }

  dmg_e amount_type( const action_state_t*, bool ) const override
  {
    return DMG_DIRECT;
  }

  action_state_t* new_state() override
  { return new am_state_t( this, target ); }

  // We need to snapshot any tick time reduction effect here so that it correctly affects the whole
  // channel.
  void snapshot_state( action_state_t* s, dmg_e rt ) override
  {
    arcane_mage_spell_t::snapshot_state( s, rt );

    if ( p()->buffs.clearcasting_channel->check() )
      debug_cast<am_state_t*>( s )->tick_time_multiplier = 1.0 + cc_tick_time_reduction;
  }

  timespan_t composite_dot_duration( const action_state_t* s ) const override
  {
    // AM channel duration is a bit fuzzy, it will go above or below the standard 2 s
    // to make sure it has the correct number of ticks.
    timespan_t full_duration = dot_duration * s->haste;

    if ( p()->buffs.clearcasting_channel->check() )
      full_duration *= 1.0 + cc_duration_reduction;

    timespan_t tick_duration = tick_time( s );
    double ticks = std::round( full_duration / tick_duration );
    return ticks * tick_duration;
  }

  timespan_t tick_time( const action_state_t* s ) const override
  {
    timespan_t t = arcane_mage_spell_t::tick_time( s );

    t *= debug_cast<const am_state_t*>( s )->tick_time_multiplier;

    return t;
  }

  void execute() override
  {
    p()->buffs.arcane_pummeling->expire();

    if ( p()->buffs.clearcasting->check() )
      p()->buffs.clearcasting_channel->trigger();
    else
      p()->buffs.clearcasting_channel->expire();

    arcane_mage_spell_t::execute();
  }

  bool usable_moving() const override
  {
    if ( p()->talents.slipstream->ok() && p()->buffs.clearcasting->check() )
      return true;

    return arcane_mage_spell_t::usable_moving();
  }

  void last_tick( dot_t* d ) override
  {
    arcane_mage_spell_t::last_tick( d );
    p()->buffs.clearcasting_channel->expire();
  }
};

// Arcane Orb Spell =========================================================

struct arcane_orb_bolt_t : public arcane_mage_spell_t
{
  arcane_orb_bolt_t( const std::string& n, mage_t* p ) :
    arcane_mage_spell_t( n, p, p->find_spell( 153640 ) )
  {
    background = true;
  }

  void impact( action_state_t* s ) override
  {
    arcane_mage_spell_t::impact( s );

    if ( result_is_hit( s->result ) )
      p()->trigger_arcane_charge();
  }
};

struct arcane_orb_t : public arcane_mage_spell_t
{
  arcane_orb_t( const std::string& n, mage_t* p, const std::string& options_str ) :
    arcane_mage_spell_t( n, p, p->talents.arcane_orb )
  {
    parse_options( options_str );
    may_miss = may_crit = false;
    aoe = -1;

    impact_action = get_action<arcane_orb_bolt_t>( "arcane_orb_bolt", p );
    add_child( impact_action );
  }

  void execute() override
  {
    arcane_mage_spell_t::execute();
    p()->trigger_arcane_charge();
  }
};

// Arcane Power Spell =======================================================

struct arcane_power_t : public arcane_mage_spell_t
{
  arcane_power_t( const std::string& n, mage_t* p, const std::string& options_str ) :
    arcane_mage_spell_t( n, p, p->find_specialization_spell( "Arcane Power" ) )
  {
    parse_options( options_str );
    harmful = false;
    cooldown->duration *= p->strive_for_perfection_multiplier;
  }

  void execute() override
  {
    arcane_mage_spell_t::execute();
    p()->buffs.arcane_power->trigger();
  }
};

// Blast Wave Spell =========================================================

struct blast_wave_t : public fire_mage_spell_t
{
  blast_wave_t( const std::string& n, mage_t* p, const std::string& options_str ) :
    fire_mage_spell_t( n, p, p->talents.blast_wave )
  {
    parse_options( options_str );
    aoe = -1;
  }
};

// Blink Spell ==============================================================

struct blink_t : public mage_spell_t
{
  blink_t( const std::string& n, mage_t* p, const std::string& options_str ) :
    mage_spell_t( n, p, p->find_class_spell( "Blink" ) )
  {
    parse_options( options_str );
    harmful = false;
    ignore_false_positive = true;
    base_teleport_distance = data().effectN( 1 ).radius_max();
    movement_directionality = MOVEMENT_OMNI;

    background = p->talents.shimmer->ok();
  }
};

// Blizzard Spell ===========================================================

struct blizzard_shard_t : public frost_mage_spell_t
{
  blizzard_shard_t( const std::string& n, mage_t* p ) :
    frost_mage_spell_t( n, p, p->find_spell( 190357 ) )
  {
    aoe = -1;
    background = ground_aoe = chills = true;
  }

  dmg_e amount_type( const action_state_t*, bool ) const override
  {
    return DMG_OVER_TIME;
  }

  void execute() override
  {
    frost_mage_spell_t::execute();

    if ( hit_any_target )
    {
      timespan_t reduction = -10 * num_targets_hit * p()->spec.blizzard_2->effectN( 1 ).time_value();
      p()->sample_data.blizzard->add( reduction );
      p()->cooldowns.frozen_orb->adjust( reduction );
    }
  }

  double action_multiplier() const override
  {
    double am = frost_mage_spell_t::action_multiplier();

    am *= 1.0 + p()->buffs.freezing_rain->check_value();

    return am;
  }
};

struct blizzard_t : public frost_mage_spell_t
{
  action_t* blizzard_shard;

  blizzard_t( const std::string& n, mage_t* p, const std::string& options_str ) :
    frost_mage_spell_t( n, p, p->find_specialization_spell( "Blizzard" ) ),
    blizzard_shard( get_action<blizzard_shard_t>( "blizzard_shard", p ) )
  {
    parse_options( options_str );
    add_child( blizzard_shard );
    cooldown->hasted = true;
    may_miss = may_crit = affected_by.shatter = false;
  }

  timespan_t execute_time() const override
  {
    if ( p()->buffs.freezing_rain->check() )
      return 0_ms;

    return frost_mage_spell_t::execute_time();
  }

  void execute() override
  {
    frost_mage_spell_t::execute();

    timespan_t ground_aoe_duration = data().duration() * player->cache.spell_speed();
    p()->ground_aoe_expiration[ name_str ] = sim->current_time() + ground_aoe_duration;

    make_event<ground_aoe_event_t>( *sim, p(), ground_aoe_params_t()
      .target( target )
      .duration( ground_aoe_duration )
      .action( blizzard_shard )
      .hasted( ground_aoe_params_t::SPELL_SPEED ), true );
  }
};

// Charged Up Spell =========================================================

struct charged_up_t : public arcane_mage_spell_t
{
  charged_up_t( const std::string& n, mage_t* p, const std::string& options_str ) :
    arcane_mage_spell_t( n, p, p->talents.charged_up )
  {
    parse_options( options_str );
    harmful = false;
  }

  void execute() override
  {
    arcane_mage_spell_t::execute();
    p()->trigger_arcane_charge( 4 );
  }
};

// Cold Snap Spell ==========================================================

struct cold_snap_t : public frost_mage_spell_t
{
  cold_snap_t( const std::string& n, mage_t* p, const std::string& options_str ) :
    frost_mage_spell_t( n, p, p->find_specialization_spell( "Cold Snap" ) )
  {
    parse_options( options_str );
    harmful = false;
  };

  void execute() override
  {
    frost_mage_spell_t::execute();

    p()->cooldowns.cone_of_cold->reset( false );
    p()->cooldowns.frost_nova->reset( false );
  }
};

// Combustion Spell =========================================================

struct combustion_t : public fire_mage_spell_t
{
  combustion_t( const std::string& n, mage_t* p, const std::string& options_str ) :
    fire_mage_spell_t( n, p, p->find_specialization_spell( "Combustion" ) )
  {
    parse_options( options_str );
    dot_duration = 0_ms;
    harmful = false;
    usable_while_casting = true;
    cooldown->duration *= p->strive_for_perfection_multiplier;
  }

  void execute() override
  {
    fire_mage_spell_t::execute();

    p()->buffs.combustion->trigger();
    p()->buffs.wildfire->trigger();
  }
};

// Comet Storm Spell ========================================================

struct comet_storm_projectile_t : public frost_mage_spell_t
{
  comet_storm_projectile_t( const std::string& n, mage_t* p ) :
    frost_mage_spell_t( n, p, p->find_spell( 153596 ) )
  {
    aoe = -1;
    background = true;
  }
};

struct comet_storm_t : public frost_mage_spell_t
{
  timespan_t delay;
  action_t* projectile;

  comet_storm_t( const std::string& n, mage_t* p, const std::string& options_str ) :
    frost_mage_spell_t( n, p, p->talents.comet_storm ),
    delay( timespan_t::from_seconds( p->find_spell( 228601 )->missile_speed() ) ),
    projectile( get_action<comet_storm_projectile_t>( "comet_storm_projectile", p ) )
  {
    parse_options( options_str );
    may_miss = may_crit = affected_by.shatter = false;
    add_child( projectile );
  }

  timespan_t travel_time() const override
  {
    return delay;
  }

  void impact( action_state_t* s ) override
  {
    frost_mage_spell_t::impact( s );

    int pulse_count = 7;
    timespan_t pulse_time = 0.2_s;
    p()->ground_aoe_expiration[ name_str ] = sim->current_time() + pulse_count * pulse_time;

    make_event<ground_aoe_event_t>( *sim, p(), ground_aoe_params_t()
      .pulse_time( pulse_time )
      .target( s->target )
      .n_pulses( pulse_count )
      .action( projectile ) );
  }
};

// Cone of Cold Spell =======================================================

struct cone_of_cold_t : public frost_mage_spell_t
{
  cone_of_cold_t( const std::string& n, mage_t* p, const std::string& options_str ) :
    frost_mage_spell_t( n, p, p->find_specialization_spell( "Cone of Cold" ) )
  {
    parse_options( options_str );
    aoe = -1;
    chills = true;
  }
};

// Conflagration Spell ======================================================

struct conflagration_t : public fire_mage_spell_t
{
  conflagration_t( const std::string& n, mage_t* p ) :
    fire_mage_spell_t( n, p, p->find_spell( 226757 ) )
  {
    background = true;
  }
};

struct conflagration_flare_up_t : public fire_mage_spell_t
{
  conflagration_flare_up_t( const std::string& n, mage_t* p ) :
    fire_mage_spell_t( n, p, p->find_spell( 205345 ) )
  {
    background = true;
    aoe = -1;
  }
};

// Counterspell Spell =======================================================

struct counterspell_t : public mage_spell_t
{
  counterspell_t( const std::string& n, mage_t* p, const std::string& options_str ) :
    mage_spell_t( n, p, p->find_class_spell( "Counterspell" ) )
  {
    parse_options( options_str );
    may_miss = may_crit = false;
    ignore_false_positive = true;
  }

  void impact( action_state_t* s ) override
  {
    mage_spell_t::impact( s );
    p()->trigger_crowd_control( s, MECHANIC_INTERRUPT );
  }

  bool target_ready( player_t* candidate_target ) override
  {
    if ( !candidate_target->debuffs.casting || !candidate_target->debuffs.casting->check() )
      return false;

    return mage_spell_t::target_ready( candidate_target );
  }
};

// Dragon's Breath Spell ====================================================

struct dragons_breath_t : public fire_mage_spell_t
{
  dragons_breath_t( const std::string& n, mage_t* p, const std::string& options_str ) :
    fire_mage_spell_t( n, p, p->find_specialization_spell( "Dragon's Breath" ) )
  {
    parse_options( options_str );
    aoe = -1;

    if ( p->talents.alexstraszas_fury->ok() )
      base_crit = 1.0;
  }

  void impact( action_state_t* s ) override
  {
    fire_mage_spell_t::impact( s );

    if ( result_is_hit( s->result ) && p()->talents.alexstraszas_fury->ok() && s->chain_target == 0 )
      handle_hot_streak( s );

    p()->trigger_crowd_control( s, MECHANIC_DISORIENT );
  }
};

// Evocation Spell ==========================================================

struct evocation_t : public arcane_mage_spell_t
{
  int brain_storm_charges;

  evocation_t( const std::string& n, mage_t* p, const std::string& options_str ) :
    arcane_mage_spell_t( n, p, p->find_specialization_spell( "Evocation" ) ),
    brain_storm_charges()
  {
    parse_options( options_str );
    base_tick_time = 1.0_s;
    dot_duration = data().duration();
    channeled = ignore_false_positive = tick_zero = true;
    harmful = false;
    cooldown->duration *= 1.0 + p->spec.evocation_2->effectN( 1 ).percent();

    if ( p->azerite.brain_storm.enabled() )
      brain_storm_charges = as<int>( p->find_spell( 288466 )->effectN( 1 ).base_value() );
  }

  void execute() override
  {
    arcane_mage_spell_t::execute();

    p()->trigger_evocation();
    if ( brain_storm_charges > 0 )
      p()->trigger_arcane_charge( brain_storm_charges );
  }

  void tick( dot_t* d ) override
  {
    arcane_mage_spell_t::tick( d );
    p()->buffs.brain_storm->trigger();
  }

  void last_tick( dot_t* d ) override
  {
    arcane_mage_spell_t::last_tick( d );
    p()->buffs.evocation->expire();
  }

  bool usable_moving() const override
  {
    if ( p()->talents.slipstream->ok() )
      return true;

    return arcane_mage_spell_t::usable_moving();
  }
};

// Ebonbolt Spell ===========================================================

struct ebonbolt_t : public frost_mage_spell_t
{
  ebonbolt_t( const std::string& n, mage_t* p, const std::string& options_str ) :
    frost_mage_spell_t( n, p, p->talents.ebonbolt )
  {
    parse_options( options_str );
    parse_effect_data( p->find_spell( 257538 )->effectN( 1 ) );
    calculate_on_impact = track_shatter = true;

    if ( p->talents.splitting_ice->ok() )
    {
      aoe = as<int>( 1 + p->talents.splitting_ice->effectN( 1 ).base_value() );
      base_aoe_multiplier *= p->talents.splitting_ice->effectN( 2 ).percent();
    }
  }

  void init_finished() override
  {
    proc_brain_freeze = p()->get_proc( "Brain Freeze from Ebonbolt" );
    frost_mage_spell_t::init_finished();
  }

  void execute() override
  {
    frost_mage_spell_t::execute();
    p()->trigger_brain_freeze( 1.0, proc_brain_freeze );
  }
};

// Fireball Spell ===========================================================

struct fireball_t : public fire_mage_spell_t
{
  fireball_t( const std::string& n, mage_t* p, const std::string& options_str ) :
    fire_mage_spell_t( n, p, p->find_class_spell( "Fireball" ) )
  {
    parse_options( options_str );
    triggers_hot_streak = triggers_ignite = triggers_kindling = true;
    base_dd_adder += p->azerite.duplicative_incineration.value( 2 );

    if ( p->talents.conflagration->ok() )
    {
      impact_action = get_action<conflagration_t>( "conflagration", p );
      add_child( impact_action );
    }
  }

  timespan_t travel_time() const override
  {
    timespan_t t = fire_mage_spell_t::travel_time();
    return std::min( t, 0.75_s );
  }

  void execute() override
  {
    fire_mage_spell_t::execute();

    if ( rng().roll( p()->azerite.duplicative_incineration.spell_ref().effectN( 1 ).percent() ) )
      execute();
  }

  void impact( action_state_t* s ) override
  {
    fire_mage_spell_t::impact( s );

    if ( result_is_hit( s->result ) )
    {
      if ( s->result == RESULT_CRIT )
        p()->buffs.enhanced_pyrotechnics->expire();
      else
        p()->buffs.enhanced_pyrotechnics->trigger();
    }
  }

  double composite_target_crit_chance( player_t* target ) const override
  {
    double c = fire_mage_spell_t::composite_target_crit_chance( target );

    if ( firestarter_active( target ) )
      c += 1.0;

    return c;
  }

  double composite_crit_chance() const override
  {
    double c = fire_mage_spell_t::composite_crit_chance();

    c += p()->buffs.enhanced_pyrotechnics->check_stack_value();

    return c;
  }
};

// Flame Patch Spell ========================================================

struct flame_patch_t : public fire_mage_spell_t
{
  flame_patch_t( const std::string& n, mage_t* p ) :
    fire_mage_spell_t( n, p, p->find_spell( 205472 ) )
  {
    aoe = -1;
    ground_aoe = background = true;
  }

  dmg_e amount_type( const action_state_t*, bool ) const override
  {
    return DMG_OVER_TIME;
  }
};

// Flamestrike Spell ========================================================

struct flamestrike_t : public hot_streak_spell_t
{
  action_t* flame_patch;
  timespan_t flame_patch_duration;

  flamestrike_t( const std::string& n, mage_t* p, const std::string& options_str ) :
    hot_streak_spell_t( n, p, p->find_specialization_spell( "Flamestrike" ) ),
    flame_patch()
  {
    parse_options( options_str );
    triggers_ignite = true;
    aoe = -1;

    if ( p->talents.flame_patch->ok() )
    {
      flame_patch = get_action<flame_patch_t>( "flame_patch", p );
      flame_patch_duration = p->find_spell( 205470 )->duration();
      add_child( flame_patch );
    }
  }

  void execute() override
  {
    hot_streak_spell_t::execute();

    if ( flame_patch )
    {
      p()->ground_aoe_expiration[ flame_patch->name_str ] = sim->current_time() + flame_patch_duration;

      make_event<ground_aoe_event_t>( *sim, p(), ground_aoe_params_t()
        .target( target )
        .duration( flame_patch_duration )
        .action( flame_patch )
        .hasted( ground_aoe_params_t::SPELL_SPEED ) );
    }
  }
};

// Flurry Spell =============================================================

struct glacial_assault_t : public frost_mage_spell_t
{
  glacial_assault_t( const std::string& n, mage_t* p ) :
    frost_mage_spell_t( n, p, p->find_spell( 279856 ) )
  {
    background = true;
    aoe = -1;
    base_dd_min = base_dd_max = p->azerite.glacial_assault.value();
  }
};

struct flurry_bolt_t : public frost_mage_spell_t
{
  double glacial_assault_chance;

  flurry_bolt_t( const std::string& n, mage_t* p ) :
    frost_mage_spell_t( n, p, p->find_spell( 228354 ) ),
    glacial_assault_chance()
  {
    background = true;
    chills = true;
    base_multiplier *= 1.0 + p->talents.lonely_winter->effectN( 1 ).percent();
    glacial_assault_chance = p->azerite.glacial_assault.spell_ref().effectN( 1 ).trigger()->proc_chance();
  }

  void impact( action_state_t* s ) override
  {
    frost_mage_spell_t::impact( s );

    if ( !result_is_hit( s->result ) )
      return;

    if ( p()->state.brain_freeze_active )
      td( s->target )->debuffs.winters_chill->trigger();

    if ( rng().roll( glacial_assault_chance ) )
    {
      // Delay is around 1 s, but the impact seems to always happen in
      // the Winter's Chill window. So here we just subtract 1 ms to make
      // sure it hits while the debuff is up.
      make_event<ground_aoe_event_t>( *sim, p(), ground_aoe_params_t()
        .pulse_time( 999_ms )
        .target( s->target )
        .n_pulses( 1 )
        .action( p()->action.glacial_assault ) );
    }
  }

  double action_multiplier() const override
  {
    double am = frost_mage_spell_t::action_multiplier();

    if ( p()->state.brain_freeze_active )
      am *= 1.0 + p()->buffs.brain_freeze->data().effectN( 2 ).percent();

    return am;
  }
};

struct flurry_t : public frost_mage_spell_t
{
  action_t* flurry_bolt;

  flurry_t( const std::string& n, mage_t* p, const std::string& options_str ) :
    frost_mage_spell_t( n, p, p->find_specialization_spell( "Flurry" ) ),
    flurry_bolt( get_action<flurry_bolt_t>( "flurry_bolt", p ) )
  {
    parse_options( options_str );
    may_miss = may_crit = affected_by.shatter = false;

    add_child( flurry_bolt );
    if ( p->spec.icicles->ok() )
      add_child( p->icicle.flurry );
    if ( p->action.glacial_assault )
      add_child( p->action.glacial_assault );
  }

  void init() override
  {
    frost_mage_spell_t::init();
    // Snapshot haste for bolt impact timing.
    snapshot_flags |= STATE_HASTE;
  }

  timespan_t execute_time() const override
  {
    if ( p()->buffs.brain_freeze->check() )
      return 0_ms;

    return frost_mage_spell_t::execute_time();
  }

  void execute() override
  {
    frost_mage_spell_t::execute();

    p()->trigger_icicle_gain( target, p()->icicle.flurry );
    if ( p()->player_t::buffs.memory_of_lucid_dreams->check() )
      p()->trigger_icicle_gain( target, p()->icicle.flurry );

    bool brain_freeze = p()->buffs.brain_freeze->up();
    p()->state.brain_freeze_active = brain_freeze;
    p()->buffs.brain_freeze->decrement();

    if ( brain_freeze )
      p()->procs.brain_freeze_used->occur();
  }

  void impact( action_state_t* s ) override
  {
    frost_mage_spell_t::impact( s );

    timespan_t pulse_time = s->haste * 0.4_s;

    make_event<ground_aoe_event_t>( *sim, p(), ground_aoe_params_t()
      .pulse_time( pulse_time )
      .target( s->target )
      .n_pulses( as<int>( data().effectN( 1 ).base_value() ) )
      .action( flurry_bolt ), true );
  }
};

// Frostbolt Spell ==========================================================

struct frostbolt_t : public frost_mage_spell_t
{
  frostbolt_t( const std::string& n, mage_t* p, const std::string& options_str ) :
    frost_mage_spell_t( n, p, p->find_specialization_spell( "Frostbolt" ) )
  {
    parse_options( options_str );
    parse_effect_data( p->find_spell( 228597 )->effectN( 1 ) );
    chills = calculate_on_impact = track_shatter = true;
    base_multiplier *= 1.0 + p->talents.lonely_winter->effectN( 1 ).percent();

    if ( p->spec.icicles->ok() )
      add_child( p->icicle.frostbolt );
  }

  void init_finished() override
  {
    proc_brain_freeze = p()->get_proc( "Brain Freeze from Frostbolt" );
    proc_fof = p()->get_proc( "Fingers of Frost from Frostbolt" );
    frost_mage_spell_t::init_finished();
  }

  void execute() override
  {
    frost_mage_spell_t::execute();

    p()->trigger_icicle_gain( target, p()->icicle.frostbolt );
    if ( p()->player_t::buffs.memory_of_lucid_dreams->check() )
      p()->trigger_icicle_gain( target, p()->icicle.frostbolt );

    double fof_proc_chance = p()->spec.fingers_of_frost->effectN( 1 ).percent();
    fof_proc_chance *= 1.0 + p()->talents.frozen_touch->effectN( 1 ).percent();
    p()->trigger_fof( fof_proc_chance, 1, proc_fof );

    double bf_proc_chance = p()->spec.brain_freeze->effectN( 1 ).percent();
    bf_proc_chance *= 1.0 + p()->talents.frozen_touch->effectN( 1 ).percent();
    p()->trigger_brain_freeze( bf_proc_chance, proc_brain_freeze );

    if ( target != p()->last_frostbolt_target )
      p()->buffs.tunnel_of_ice->expire();
    p()->last_frostbolt_target = target;
  }

  void impact( action_state_t* s ) override
  {
    frost_mage_spell_t::impact( s );

    if ( result_is_hit( s->result ) )
      p()->buffs.tunnel_of_ice->trigger();
  }

  double bonus_da( const action_state_t* s ) const override
  {
    double da = frost_mage_spell_t::bonus_da( s );

    da += p()->buffs.tunnel_of_ice->check_stack_value();

    return da;
  }
};

// Frost Nova Spell =========================================================

struct frost_nova_t : public mage_spell_t
{
  frost_nova_t( const std::string& n, mage_t* p, const std::string& options_str ) :
    mage_spell_t( n, p, p->find_class_spell( "Frost Nova" ) )
  {
    parse_options( options_str );
    aoe = -1;
    affected_by.shatter = true;
    cooldown->charges += as<int>( p->talents.ice_ward->effectN( 1 ).base_value() );
  }

  void impact( action_state_t* s ) override
  {
    mage_spell_t::impact( s );
    p()->trigger_crowd_control( s, MECHANIC_ROOT );
  }
};

// Frozen Orb Spell =========================================================

struct frozen_orb_bolt_t : public frost_mage_spell_t
{
  frozen_orb_bolt_t( const std::string& n, mage_t* p ) :
    frost_mage_spell_t( n, p, p->find_spell( 84721 ) )
  {
    aoe = -1;
    background = chills = true;
  }

  void init_finished() override
  {
    proc_fof = p()->get_proc( "Fingers of Frost from Frozen Orb Tick" );
    frost_mage_spell_t::init_finished();
  }

  void execute() override
  {
    frost_mage_spell_t::execute();

    if ( hit_any_target )
      p()->trigger_fof( p()->spec.fingers_of_frost->effectN( 2 ).percent(), 1, proc_fof );
  }

  double action_multiplier() const override
  {
    double am = frost_mage_spell_t::action_multiplier();

    am *= 1.0 + p()->cache.mastery() * p()->spec.icicles->effectN( 4 ).mastery_value();

    return am;
  }

  void impact( action_state_t* s ) override
  {
    frost_mage_spell_t::impact( s );

    if ( result_is_hit( s->result ) )
      td( s->target )->debuffs.packed_ice->trigger();
  }
};

struct frozen_orb_t : public frost_mage_spell_t
{
  action_t* frozen_orb_bolt;

  frozen_orb_t( const std::string& n, mage_t* p, const std::string& options_str ) :
    frost_mage_spell_t( n, p, p->find_specialization_spell( "Frozen Orb" ) ),
    frozen_orb_bolt( get_action<frozen_orb_bolt_t>( "frozen_orb_bolt", p ) )
  {
    parse_options( options_str );
    may_miss = may_crit = affected_by.shatter = false;
    add_child( frozen_orb_bolt );
  }

  void init_finished() override
  {
    proc_fof = p()->get_proc( "Fingers of Frost from Frozen Orb Initial Impact" );
    frost_mage_spell_t::init_finished();
  }

  timespan_t travel_time() const override
  {
    timespan_t t = frost_mage_spell_t::travel_time();

    // Frozen Orb activates after about 0.5 s, even in melee range.
    t = std::max( t, 0.5_s );

    return t;
  }

  void execute() override
  {
    frost_mage_spell_t::execute();
    p()->buffs.freezing_rain->trigger();
  }

  void impact( action_state_t* s ) override
  {
    frost_mage_spell_t::impact( s );
    p()->trigger_fof( 1.0, 1, proc_fof );

    int pulse_count = 20;
    timespan_t pulse_time = 0.5_s;
    p()->ground_aoe_expiration[ name_str ] = sim->current_time() + ( pulse_count - 1 ) * pulse_time;

    make_event<ground_aoe_event_t>( *sim, p(), ground_aoe_params_t()
      .pulse_time( pulse_time )
      .target( s->target )
      .n_pulses( pulse_count )
      .action( frozen_orb_bolt ), true );
  }
};

// Glacial Spike Spell ======================================================

struct glacial_spike_t : public frost_mage_spell_t
{
  glacial_spike_t( const std::string& n, mage_t* p, const std::string& options_str ) :
    frost_mage_spell_t( n, p, p->talents.glacial_spike )
  {
    parse_options( options_str );
    parse_effect_data( p->find_spell( 228600 )->effectN( 1 ) );
    calculate_on_impact = track_shatter = true;
    base_dd_adder += p->azerite.flash_freeze.value( 2 ) * p->spec.icicles->effectN( 2 ).base_value();

    if ( p->talents.splitting_ice->ok() )
    {
      aoe = as<int>( 1 + p->talents.splitting_ice->effectN( 1 ).base_value() );
      base_aoe_multiplier *= p->talents.splitting_ice->effectN( 2 ).percent();
    }
  }

  void init_finished() override
  {
    proc_fof = p()->get_proc( "Fingers of Frost from Flash Freeze" );
    frost_mage_spell_t::init_finished();
  }

  bool ready() override
  {
    // Glacial Spike doesn't check the Icicles buff after it started executing.
    if ( p()->executing != this && p()->buffs.icicles->check() < p()->buffs.icicles->max_stack() )
      return false;

    return frost_mage_spell_t::ready();
  }

  double action_multiplier() const override
  {
    double am = frost_mage_spell_t::action_multiplier();

    double icicle_coef = icicle_sp_coefficient();
    icicle_coef *=       p()->spec.icicles->effectN( 2 ).base_value();
    icicle_coef *= 1.0 + p()->talents.splitting_ice->effectN( 3 ).percent();

    // The damage from Icicles is added as multiplier that corresponds to
    // 1 + Icicle damage / base damage, for some reason.
    //
    // TODO: This causes mastery to affect Flash Freeze bonus damage and
    // therefore might not be intended.
    am *= 1.0 + icicle_coef / spell_power_mod.direct;

    return am;
  }

  void execute() override
  {
    frost_mage_spell_t::execute();

    p()->buffs.icicles->expire();
    while ( !p()->icicles.empty() )
      p()->get_icicle();

    double fof_proc_chance = p()->azerite.flash_freeze.spell_ref().effectN( 1 ).percent();
    for ( int i = 0; i < as<int>( p()->spec.icicles->effectN( 2 ).base_value() ); i++ )
      p()->trigger_fof( fof_proc_chance, 1, proc_fof );
  }

  void impact( action_state_t* s ) override
  {
    frost_mage_spell_t::impact( s );
    p()->trigger_crowd_control( s, MECHANIC_ROOT );
  }
};

// Ice Floes Spell ==========================================================

struct ice_floes_t : public mage_spell_t
{
  ice_floes_t( const std::string& n, mage_t* p, const std::string& options_str ) :
    mage_spell_t( n, p, p->talents.ice_floes )
  {
    parse_options( options_str );
    may_miss = may_crit = harmful = false;
    usable_while_casting = true;
    internal_cooldown->duration = data().internal_cooldown();
  }

  void execute() override
  {
    mage_spell_t::execute();
    p()->buffs.ice_floes->trigger();
  }
};

// Ice Lance Spell ==========================================================

struct ice_lance_state_t : public mage_spell_state_t
{
  bool fingers_of_frost;

  ice_lance_state_t( action_t* action, player_t* target ) :
    mage_spell_state_t( action, target ),
    fingers_of_frost()
  { }

  void initialize() override
  {
    mage_spell_state_t::initialize();
    fingers_of_frost = false;
  }

  std::ostringstream& debug_str( std::ostringstream& s ) override
  {
    mage_spell_state_t::debug_str( s ) << " fingers_of_frost=" << fingers_of_frost;
    return s;
  }

  void copy_state( const action_state_t* s ) override
  {
    mage_spell_state_t::copy_state( s );
    fingers_of_frost = debug_cast<const ice_lance_state_t*>( s )->fingers_of_frost;
  }
};

struct ice_lance_t : public frost_mage_spell_t
{
  shatter_source_t* extension_source;
  shatter_source_t* cleave_source;

  ice_lance_t( const std::string& n, mage_t* p, const std::string& options_str ) :
    frost_mage_spell_t( n, p, p->find_specialization_spell( "Ice Lance" ) ),
    extension_source(),
    cleave_source()
  {
    parse_options( options_str );
    parse_effect_data( p->find_spell( 228598 )->effectN( 1 ) );
    calculate_on_impact = track_shatter = true;
    base_multiplier *= 1.0 + p->talents.lonely_winter->effectN( 1 ).percent();
    base_dd_adder += p->azerite.whiteout.value( 3 );

    // TODO: Cleave distance for SI seems to be 8 + hitbox size.
    if ( p->talents.splitting_ice->ok() )
    {
      aoe = as<int>( 1 + p->talents.splitting_ice->effectN( 1 ).base_value() );
      base_multiplier *= 1.0 + p->talents.splitting_ice->effectN( 3 ).percent();
      base_aoe_multiplier *= p->talents.splitting_ice->effectN( 2 ).percent();
    }
  }

  void init_finished() override
  {
    frost_mage_spell_t::init_finished();

    if ( sim->report_details != 0 && p()->talents.splitting_ice->ok() )
      cleave_source = p()->get_shatter_source( "Ice Lance cleave" );
    if ( sim->report_details != 0 && p()->talents.thermal_void->ok() )
      extension_source = p()->get_shatter_source( "Thermal Void extension" );
  }

  action_state_t* new_state() override
  { return new ice_lance_state_t( this, target ); }

  unsigned frozen( const action_state_t* s ) const override
  {
    unsigned source = frost_mage_spell_t::frozen( s );

    // In game, FoF Ice Lances are implemented using a global flag which determines
    // whether to treat the targets as frozen or not. On IL execute, FoF is checked
    // and the flag set accordingly.
    //
    // This works fine under normal circumstances. However, once GCD drops below IL's
    // travel time, it's possible to:
    //
    //   a) cast FoF IL, cast non-FoF IL before the first one impacts
    //   b) cast non-FoF IL, cast FoF IL before the first one impacts
    //
    // In the a) case, neither Ice Lance gets the extra damage/Shatter bonus, in the
    // b) case, both Ice Lances do.
    if ( p()->bugs )
    {
      if ( p()->state.fingers_of_frost_active )
        source |= FF_FINGERS_OF_FROST;
    }
    else
    {
      if ( debug_cast<const ice_lance_state_t*>( s )->fingers_of_frost )
        source |= FF_FINGERS_OF_FROST;
    }

    return source;
  }

  void execute() override
  {
    p()->state.fingers_of_frost_active = p()->buffs.fingers_of_frost->up();

    frost_mage_spell_t::execute();

    p()->buffs.fingers_of_frost->decrement();

    // Begin casting all Icicles at the target, beginning 0.25 seconds after the
    // Ice Lance cast with remaining Icicles launching at intervals of 0.4
    // seconds, the latter adjusted by haste. Casting continues until all
    // Icicles are gone, including new ones that accumulate while they're being
    // fired. If target dies, Icicles stop.
    if ( !p()->talents.glacial_spike->ok() )
      p()->trigger_icicle( target, true );

    if ( p()->azerite.whiteout.enabled() )
    {
      p()->cooldowns.frozen_orb
         ->adjust( -100 * p()->azerite.whiteout.spell_ref().effectN( 2 ).time_value(), false );
    }
  }

  void snapshot_state( action_state_t* s, dmg_e rt ) override
  {
    debug_cast<ice_lance_state_t*>( s )->fingers_of_frost = p()->buffs.fingers_of_frost->check() != 0;
    frost_mage_spell_t::snapshot_state( s, rt );
  }

  timespan_t travel_time() const override
  {
    timespan_t t = frost_mage_spell_t::travel_time();

    if ( p()->options.allow_shimmer_lance && p()->buffs.shimmer->check() )
    {
      double shimmer_distance = p()->talents.shimmer->effectN( 1 ).radius_max();
      t = std::max( t - timespan_t::from_seconds( shimmer_distance / travel_speed ), 1_ms );
    }

    return t;
  }

  void impact( action_state_t* s ) override
  {
    frost_mage_spell_t::impact( s );

    if ( !result_is_hit( s->result ) )
      return;

    bool primary = s->chain_target == 0;
    unsigned frozen = cast_state( s )->frozen;

    if ( primary && frozen )
    {
      if ( p()->talents.thermal_void->ok() && p()->buffs.icy_veins->check() )
      {
        p()->buffs.icy_veins
           ->extend_duration( p(), 1000 * p()->talents.thermal_void->effectN( 1 ).time_value() );

        record_shatter_source( s, extension_source );
      }

      if ( frozen &  FF_FINGERS_OF_FROST
        && frozen & ~FF_FINGERS_OF_FROST )
      {
        p()->procs.fingers_of_frost_wasted->occur();
      }

      p()->buffs.chain_reaction->trigger();
    }

    if ( !primary )
      record_shatter_source( s, cleave_source );
  }

  double action_multiplier() const override
  {
    double am = frost_mage_spell_t::action_multiplier();

    am *= 1.0 + p()->buffs.chain_reaction->check_stack_value();

    return am;
  }

  double frozen_multiplier( const action_state_t* s ) const override
  {
    double m = frost_mage_spell_t::frozen_multiplier( s );

    m *= 3.0;

    return m;
  }

  double bonus_da( const action_state_t* s ) const override
  {
    double da = frost_mage_spell_t::bonus_da( s );

    if ( auto td = p()->target_data[ s->target ] )
    {
      double pi_bonus = td->debuffs.packed_ice->check_value();

      // Splitting Ice nerfs this trait by 33%, see:
      // https://us.battle.net/forums/en/wow/topic/20769009293#post-1
      if ( num_targets_hit > 1 )
        pi_bonus *= 0.666;

      da += pi_bonus;
    }

    return da;
  }
};

// Ice Nova Spell ===========================================================

struct ice_nova_t : public frost_mage_spell_t
{
  ice_nova_t( const std::string& n, mage_t* p, const std::string& options_str ) :
     frost_mage_spell_t( n, p, p->talents.ice_nova )
  {
    parse_options( options_str );
    aoe = -1;

    double in_mult = p->talents.ice_nova->effectN( 3 ).percent();
    base_multiplier     *= in_mult;
    base_aoe_multiplier /= in_mult;
  }

  void impact( action_state_t* s ) override
  {
    frost_mage_spell_t::impact( s );
    p()->trigger_crowd_control( s, MECHANIC_ROOT );
  }
};

// Icy Veins Spell ==========================================================

struct icy_veins_t : public frost_mage_spell_t
{
  bool precombat;

  icy_veins_t( const std::string& n, mage_t* p, const std::string& options_str ) :
    frost_mage_spell_t( n, p, p->find_specialization_spell( "Icy Veins" ) ),
    precombat()
  {
    parse_options( options_str );
    harmful = false;
    cooldown->duration *= p->strive_for_perfection_multiplier;
  }

  void init_finished() override
  {
    frost_mage_spell_t::init_finished();

    if ( action_list->name_str == "precombat" )
      precombat = true;
  }

  void schedule_execute( action_state_t* s ) override
  {
    // Icy Veins buff is applied before the spell is cast, allowing it to
    // reduce GCD of the action that triggered it.
    if ( !precombat )
      p()->buffs.icy_veins->trigger();

    frost_mage_spell_t::schedule_execute( s );
  }

  void execute() override
  {
    frost_mage_spell_t::execute();

    // Precombat actions skip schedule_execute, so the buff needs to be
    // triggered here for precombat actions.
    if ( precombat )
      p()->buffs.icy_veins->trigger();

    // Frigid Grasp buff doesn't refresh.
    p()->buffs.frigid_grasp->expire();
    p()->buffs.frigid_grasp->trigger();
  }
};

// Fire Blast Spell =========================================================

struct fire_blast_t : public fire_mage_spell_t
{
  fire_blast_t( const std::string& n, mage_t* p, const std::string& options_str ) :
    fire_mage_spell_t( n, p, p->find_specialization_spell( "Fire Blast" ) )
  {
    parse_options( options_str );
    usable_while_casting = true;
    triggers_hot_streak = triggers_ignite = triggers_kindling = true;

    cooldown->charges += as<int>( p->spec.fire_blast_3->effectN( 1 ).base_value() );
    cooldown->charges += as<int>( p->talents.flame_on->effectN( 1 ).base_value() );
    cooldown->duration -= 1000 * p->talents.flame_on->effectN( 3 ).time_value();
    cooldown->hasted = true;

    base_crit += p->spec.fire_blast_2->effectN( 1 ).percent();
  }

  void execute() override
  {
    fire_mage_spell_t::execute();
    p()->buffs.blaster_master->trigger();
  }

  double recharge_multiplier( const cooldown_t& cd ) const override
  {
    double m = fire_mage_spell_t::recharge_multiplier( cd );

    if ( &cd == cooldown && p()->player_t::buffs.memory_of_lucid_dreams->check() )
      m /= 1.0 + p()->player_t::buffs.memory_of_lucid_dreams->data().effectN( 1 ).percent();

    return m;
  }
};

// Living Bomb Spell ========================================================

struct living_bomb_dot_t : public fire_mage_spell_t
{
  // The game has two spells for the DoT, one for pre-spread one and one for
  // post-spread one. This allows two copies of the DoT to be up on one target.
  const bool primary;

  static unsigned dot_spell_id( bool primary )
  { return primary ? 217694 : 244813; }

  living_bomb_dot_t( const std::string& n, mage_t* p, bool primary_ ) :
    fire_mage_spell_t( n, p, p->find_spell( dot_spell_id( primary_ ) ) ),
    primary( primary_ )
  {
    background = true;
  }

  void init() override
  {
    fire_mage_spell_t::init();
    update_flags &= ~STATE_HASTE;
  }

  timespan_t composite_dot_duration( const action_state_t* s ) const override
  {
    return dot_duration * ( tick_time( s ) / base_tick_time );
  }

  void trigger_explosion( player_t* target )
  {
    p()->action.living_bomb_explosion->set_target( target );

    if ( primary )
    {
      for ( auto t : p()->action.living_bomb_explosion->target_list() )
      {
        if ( t == target )
          continue;

        p()->action.living_bomb_dot_spread->set_target( t );
        p()->action.living_bomb_dot_spread->execute();
      }
    }

    p()->action.living_bomb_explosion->execute();
  }

  void trigger_dot( action_state_t* s ) override
  {
    if ( get_dot( s->target )->is_ticking() )
      trigger_explosion( s->target );

    fire_mage_spell_t::trigger_dot( s );
  }

  void last_tick( dot_t* d ) override
  {
    fire_mage_spell_t::last_tick( d );
    trigger_explosion( d->target );
  }
};

struct living_bomb_explosion_t : public fire_mage_spell_t
{
  living_bomb_explosion_t( const std::string& n, mage_t* p ) :
    fire_mage_spell_t( n, p, p->find_spell( 44461 ) )
  {
    aoe = -1;
    background = true;
  }
};

struct living_bomb_t : public fire_mage_spell_t
{
  living_bomb_t( const std::string& n, mage_t* p, const std::string& options_str ) :
    fire_mage_spell_t( n, p, p->talents.living_bomb )
  {
    parse_options( options_str );
    cooldown->hasted = true;
    may_miss = may_crit = false;
    impact_action = p->action.living_bomb_dot;

    if ( data().ok() )
    {
      add_child( p->action.living_bomb_dot );
      add_child( p->action.living_bomb_dot_spread );
      add_child( p->action.living_bomb_explosion );
    }
  }
};

// Meteor Spell =============================================================

// Implementation details from Celestalon:
// http://blue.mmo-champion.com/topic/318876-warlords-of-draenor-theorycraft-discussion/#post301
// Meteor is split over a number of spell IDs
// - Meteor (id=153561) is the talent spell, the driver
// - Meteor (id=153564) is the initial impact damage
// - Meteor Burn (id=155158) is the ground effect tick damage
// - Meteor Burn (id=175396) provides the tooltip's burn duration
// - Meteor (id=177345) contains the time between cast and impact
struct meteor_burn_t : public fire_mage_spell_t
{
  meteor_burn_t( const std::string& n, mage_t* p ) :
    fire_mage_spell_t( n, p, p->find_spell( 155158 ) )
  {
    background = ground_aoe = true;
    aoe = -1;
    std::swap( spell_power_mod.direct, spell_power_mod.tick );
    dot_duration = 0_ms;
    radius = p->find_spell( 153564 )->effectN( 1 ).radius_max();
  }

  dmg_e amount_type( const action_state_t*, bool ) const override
  {
    return DMG_OVER_TIME;
  }
};

struct meteor_impact_t : public fire_mage_spell_t
{
  timespan_t meteor_burn_duration;
  timespan_t meteor_burn_pulse_time;

  meteor_impact_t( const std::string& n, mage_t* p ) :
    fire_mage_spell_t( n, p, p->find_spell( 153564 ) ),
    meteor_burn_duration( p->find_spell( 175396 )->duration() ),
    meteor_burn_pulse_time( p->find_spell( 155158 )->effectN( 1 ).period() )
  {
    background = split_aoe_damage = true;
    aoe = -1;
    triggers_ignite = true;
  }

  timespan_t travel_time() const override
  {
    return timespan_t::from_seconds( data().missile_speed() );
  }

  void impact( action_state_t* s ) override
  {
    fire_mage_spell_t::impact( s );

    if ( s->chain_target == 0 )
    {
      p()->ground_aoe_expiration[ p()->action.meteor_burn->name_str ] = sim->current_time() + meteor_burn_duration;

      make_event<ground_aoe_event_t>( *sim, p(), ground_aoe_params_t()
        .pulse_time( meteor_burn_pulse_time )
        .target( s->target )
        .duration( meteor_burn_duration )
        .action( p()->action.meteor_burn ) );
    }
  }
};

struct meteor_t : public fire_mage_spell_t
{
  timespan_t meteor_delay;

  meteor_t( const std::string& n, mage_t* p, const std::string& options_str ) :
    fire_mage_spell_t( n, p, p->talents.meteor ),
    meteor_delay( p->find_spell( 177345 )->duration() )
  {
    parse_options( options_str );
    impact_action = p->action.meteor_impact;

    if ( data().ok() )
    {
      add_child( p->action.meteor_burn );
      add_child( p->action.meteor_impact );
    }
  }

  timespan_t travel_time() const override
  {
    timespan_t impact_time = meteor_delay * p()->cache.spell_speed();
    timespan_t meteor_spawn = impact_time - impact_action->travel_time();
    return std::max( meteor_spawn, 0_ms );
  }
};

// Mirror Image Spell =======================================================

struct mirror_image_t : public mage_spell_t
{
  mirror_image_t( const std::string& n, mage_t* p, const std::string& options_str ) :
    mage_spell_t( n, p, p->talents.mirror_image )
  {
    parse_options( options_str );
    harmful = false;
  }

  void init_finished() override
  {
    for ( auto image : p()->pets.mirror_images )
    {
      for ( auto a : image->action_list )
        add_child( a );
    }

    mage_spell_t::init_finished();
  }

  void execute() override
  {
    mage_spell_t::execute();

    for ( auto image : p()->pets.mirror_images )
      image->summon( data().duration() );
  }
};

// Nether Tempest Spell =====================================================

struct nether_tempest_aoe_t : public arcane_mage_spell_t
{
  nether_tempest_aoe_t( const std::string& n, mage_t* p ) :
    arcane_mage_spell_t( n, p, p->find_spell( 114954 ) )
  {
    aoe = -1;
    background = true;
  }

  dmg_e amount_type( const action_state_t*, bool ) const override
  {
    return DMG_OVER_TIME;
  }

  timespan_t travel_time() const override
  {
    return timespan_t::from_seconds( data().missile_speed() );
  }
};

struct nether_tempest_t : public arcane_mage_spell_t
{
  action_t* nether_tempest_aoe;

  nether_tempest_t( const std::string& n, mage_t* p, const std::string& options_str ) :
    arcane_mage_spell_t( n, p, p->talents.nether_tempest ),
    nether_tempest_aoe( get_action<nether_tempest_aoe_t>( "nether_tempest_aoe", p ) )
  {
    parse_options( options_str );
    add_child( nether_tempest_aoe );
  }

  void execute() override
  {
    p()->benefits.arcane_charge.nether_tempest->update();

    arcane_mage_spell_t::execute();

    if ( hit_any_target )
    {
      if ( p()->last_bomb_target && p()->last_bomb_target != target )
        td( p()->last_bomb_target )->dots.nether_tempest->cancel();
      p()->last_bomb_target = target;
    }
  }

  void tick( dot_t* d ) override
  {
    arcane_mage_spell_t::tick( d );

    action_state_t* aoe_state = nether_tempest_aoe->get_state();
    aoe_state->target = d->target;
    nether_tempest_aoe->snapshot_state( aoe_state, nether_tempest_aoe->amount_type( aoe_state ) );

    aoe_state->persistent_multiplier *= d->state->persistent_multiplier;
    aoe_state->da_multiplier *= d->get_last_tick_factor();
    aoe_state->ta_multiplier *= d->get_last_tick_factor();

    nether_tempest_aoe->schedule_execute( aoe_state );
  }

  double composite_persistent_multiplier( const action_state_t* s ) const override
  {
    double m = arcane_mage_spell_t::composite_persistent_multiplier( s );

    m *= arcane_charge_multiplier();

    return m;
  }
};

// Phoenix Flames Spell =====================================================

struct phoenix_flames_splash_t : public fire_mage_spell_t
{
  phoenix_flames_splash_t( const std::string& n, mage_t* p ) :
    fire_mage_spell_t( n, p, p->find_spell( 257542 ) )
  {
    aoe = -1;
    background = true;
    triggers_ignite = true;
    // Phoenix Flames always crits
    base_crit = 1.0;
  }

  size_t available_targets( std::vector<player_t*>& tl ) const override
  {
    fire_mage_spell_t::available_targets( tl );

    tl.erase( std::remove( tl.begin(), tl.end(), target ), tl.end() );

    return tl.size();
  }
};

struct phoenix_flames_t : public fire_mage_spell_t
{
  phoenix_flames_t( const std::string& n, mage_t* p, const std::string& options_str ) :
    fire_mage_spell_t( n, p, p->talents.phoenix_flames )
  {
    parse_options( options_str );
    triggers_hot_streak = triggers_ignite = triggers_kindling = true;
    // Phoenix Flames always crits
    base_crit = 1.0;

    impact_action = get_action<phoenix_flames_splash_t>( "phoenix_flames_splash", p );
    add_child( impact_action );
  }

  timespan_t travel_time() const override
  {
    timespan_t t = fire_mage_spell_t::travel_time();
    return std::min( t, 0.75_s );
  }
};

// Pyroblast Spell ==========================================================

struct trailing_embers_t : public fire_mage_spell_t
{
  trailing_embers_t( const std::string& n, mage_t* p ) :
    fire_mage_spell_t( n, p, p->find_spell( 277703 ) )
  {
    background = tick_zero = true;
    hasted_ticks = false;
    base_td = p->azerite.trailing_embers.value();
  }
};

struct pyroblast_t : public hot_streak_spell_t
{
  action_t* trailing_embers;

  pyroblast_t( const std::string& n, mage_t* p, const std::string& options_str ) :
    hot_streak_spell_t( n, p, p->find_specialization_spell( "Pyroblast" ) ),
    trailing_embers()
  {
    parse_options( options_str );
    triggers_hot_streak = triggers_ignite = triggers_kindling = true;
    base_dd_adder += p->azerite.wildfire.value( 2 );

    if ( p->azerite.trailing_embers.enabled() )
    {
      trailing_embers = get_action<trailing_embers_t>( "trailing_embers", p );
      add_child( trailing_embers );
    }
  }

  double action_multiplier() const override
  {
    double am = hot_streak_spell_t::action_multiplier();

    if ( !last_hot_streak )
      am *= 1.0 + p()->buffs.pyroclasm->check_value();

    return am;
  }

  void execute() override
  {
    hot_streak_spell_t::execute();

    if ( !last_hot_streak )
      p()->buffs.pyroclasm->decrement();
  }

  timespan_t travel_time() const override
  {
    timespan_t t = hot_streak_spell_t::travel_time();
    return std::min( t, 0.75_s );
  }

  void impact( action_state_t* s ) override
  {
    hot_streak_spell_t::impact( s );

    if ( trailing_embers )
    {
      for ( auto t : target_list() )
      {
        trailing_embers->set_target( t );
        trailing_embers->execute();
      }
    }
  }

  double composite_target_crit_chance( player_t* target ) const override
  {
    double c = hot_streak_spell_t::composite_target_crit_chance( target );

    if ( firestarter_active( target ) )
      c += 1.0;

    return c;
  }
};

// Ray of Frost Spell =======================================================

struct ray_of_frost_t : public frost_mage_spell_t
{
  ray_of_frost_t( const std::string& n, mage_t* p, const std::string& options_str ) :
    frost_mage_spell_t( n, p, p->talents.ray_of_frost )
  {
    parse_options( options_str );
    channeled = chills = true;
  }

  void init_finished() override
  {
    proc_fof = p()->get_proc( "Fingers of Frost from Ray of Frost" );
    frost_mage_spell_t::init_finished();
  }

  void tick( dot_t* d ) override
  {
    frost_mage_spell_t::tick( d );
    p()->buffs.ray_of_frost->trigger();

    // Ray of Frost triggers Bone Chilling on each tick, as well as on execute.
    p()->buffs.bone_chilling->trigger();

    // TODO: Now happens at 2.5 and 5.
    if ( d->current_tick == 3 || d->current_tick == 5 )
      p()->trigger_fof( 1.0, 1, proc_fof );
  }

  void last_tick( dot_t* d ) override
  {
    frost_mage_spell_t::last_tick( d );
    p()->buffs.ray_of_frost->expire();
  }

  double action_multiplier() const override
  {
    double am = frost_mage_spell_t::action_multiplier();

    am *= 1.0 + p()->buffs.ray_of_frost->check_stack_value();

    return am;
  }
};

// Rune of Power Spell ======================================================

struct rune_of_power_t : public mage_spell_t
{
  rune_of_power_t( const std::string& n, mage_t* p, const std::string& options_str ) :
    mage_spell_t( n, p, p->talents.rune_of_power )
  {
    parse_options( options_str );
    harmful = false;
  }

  void execute() override
  {
    mage_spell_t::execute();

    p()->distance_from_rune = 0;
    p()->buffs.rune_of_power->trigger();
  }
};

// Scorch Spell =============================================================

struct scorch_t : public fire_mage_spell_t
{
  scorch_t( const std::string& n, mage_t* p, const std::string& options_str ) :
    fire_mage_spell_t( n, p, p->find_specialization_spell( "Scorch" ) )
  {
    parse_options( options_str );
    triggers_hot_streak = triggers_ignite = true;
  }

  double composite_da_multiplier( const action_state_t* s ) const override
  {
    double m = fire_mage_spell_t::composite_da_multiplier( s );

    if ( s->target->health_percentage() < p()->talents.searing_touch->effectN( 1 ).base_value() )
      m *= 1.0 + p()->talents.searing_touch->effectN( 2 ).percent();

    return m;
  }

  double composite_target_crit_chance( player_t* target ) const override
  {
    double c = fire_mage_spell_t::composite_target_crit_chance( target );

    if ( target->health_percentage() < p()->talents.searing_touch->effectN( 1 ).base_value() )
      c += 1.0;

    return c;
  }

  void impact( action_state_t* s ) override
  {
    fire_mage_spell_t::impact( s );

    if ( result_is_hit( s->result ) )
      p()->buffs.frenetic_speed->trigger();
  }

  timespan_t travel_time() const override
  {
    return fire_mage_spell_t::travel_time() + p()->options.scorch_delay;
  }

  bool usable_moving() const override
  { return true; }
};

// Shimmer Spell ============================================================

struct shimmer_t : public mage_spell_t
{
  shimmer_t( const std::string& n, mage_t* p, const std::string& options_str ) :
    mage_spell_t( n, p, p->talents.shimmer )
  {
    parse_options( options_str );
    harmful = false;
    ignore_false_positive = usable_while_casting = true;
    base_teleport_distance = data().effectN( 1 ).radius_max();
    movement_directionality = MOVEMENT_OMNI;
  }

  void execute() override
  {
    mage_spell_t::execute();
    p()->buffs.shimmer->trigger();
  }
};

// Slow Spell ===============================================================

struct slow_t : public arcane_mage_spell_t
{
  slow_t( const std::string& n, mage_t* p, const std::string& options_str ) :
    arcane_mage_spell_t( n, p, p->find_specialization_spell( "Slow" ) )
  {
    parse_options( options_str );
    ignore_false_positive = true;
  }
};

// Supernova Spell ==========================================================

struct supernova_t : public arcane_mage_spell_t
{
  supernova_t( const std::string& n, mage_t* p, const std::string& options_str ) :
    arcane_mage_spell_t( n, p, p->talents.supernova )
  {
    parse_options( options_str );
    aoe = -1;

    double sn_mult = 1.0 + p->talents.supernova->effectN( 1 ).percent();
    base_multiplier     *= sn_mult;
    base_aoe_multiplier /= sn_mult;
  }
};

// Summon Water Elemental Spell =============================================

struct summon_water_elemental_t : public frost_mage_spell_t
{
  summon_water_elemental_t( const std::string& n, mage_t* p, const std::string& options_str ) :
    frost_mage_spell_t( n, p, p->find_specialization_spell( "Summon Water Elemental" ) )
  {
    parse_options( options_str );
    harmful = track_cd_waste = false;
    ignore_false_positive = true;
    background = p->talents.lonely_winter->ok();
  }

  void execute() override
  {
    frost_mage_spell_t::execute();
    p()->pets.water_elemental->summon();
  }

  bool ready() override
  {
    if ( !p()->pets.water_elemental || !p()->pets.water_elemental->is_sleeping() )
      return false;

    return frost_mage_spell_t::ready();
  }
};

// Time Warp Spell ==========================================================

struct time_warp_t : public mage_spell_t
{
  time_warp_t( const std::string& n, mage_t* p, const std::string& options_str ) :
    mage_spell_t( n, p, p->find_class_spell( "Time Warp" ) )
  {
    parse_options( options_str );
    harmful = false;
    background = sim->overrides.bloodlust != 0;
  }

  void execute() override
  {
    mage_spell_t::execute();

    for ( player_t* p : sim->player_non_sleeping_list )
    {
      if ( p->buffs.exhaustion->check() || p->is_pet() )
        continue;

      p->buffs.bloodlust->trigger();
      p->buffs.exhaustion->trigger();
    }
  }

  bool ready() override
  {
    if ( player->buffs.exhaustion->check() )
      return false;

    return mage_spell_t::ready();
  }
};

// Touch of the Magi Spell ==================================================

struct touch_of_the_magi_t : public arcane_mage_spell_t
{
  touch_of_the_magi_t( const std::string& n, mage_t* p ) :
    arcane_mage_spell_t( n, p, p->find_spell( 210833 ) )
  {
    background = true;
    may_miss = may_crit = callbacks = false;
    aoe = -1;
    base_dd_min = base_dd_max = 1.0;
  }

  void init() override
  {
    arcane_mage_spell_t::init();

    snapshot_flags &= STATE_NO_MULTIPLIER;
    snapshot_flags |= STATE_TGT_MUL_DA;
  }

  double composite_target_multiplier( player_t* target ) const override
  {
    double m = arcane_mage_spell_t::composite_target_multiplier( target );

    // It seems that TotM explosion only double dips on target based damage reductions
    // and not target based damage increases.
    m = std::min( m, 1.0 );

    return m;
  }
};

// ==========================================================================
// Mage Custom Actions
// ==========================================================================

// Arcane Mage "Burn" State Switch Action ===================================

void report_burn_switch_error( action_t* a )
{
  throw std::runtime_error(
    fmt::format( "{} action {} infinite loop detected (no time passing between executes) at '{}'",
                 a->player->name(), a->name(), a->signature_str ) );
}

struct start_burn_phase_t : public action_t
{
  start_burn_phase_t( const std::string& n, mage_t* p, const std::string& options_str ) :
    action_t( ACTION_OTHER, n, p )
  {
    parse_options( options_str );
    trigger_gcd = 0_ms;
    harmful = false;
    ignore_false_positive = true;
  }

  void execute() override
  {
    mage_t* p = debug_cast<mage_t*>( player );

    bool success = p->burn_phase.enable( sim->current_time() );
    if ( !success )
      report_burn_switch_error( this );

    p->sample_data.burn_initial_mana->add( 100.0 * p->resources.pct( RESOURCE_MANA ) );
    p->uptime.burn_phase->update( true, sim->current_time() );
    p->uptime.conserve_phase->update( false, sim->current_time() );
  }

  bool ready() override
  {
    if ( debug_cast<mage_t*>( player )->burn_phase.on() )
      return false;

    return action_t::ready();
  }
};

struct stop_burn_phase_t : public action_t
{
  stop_burn_phase_t( const std::string& n, mage_t* p, const std::string& options_str ) :
    action_t( ACTION_OTHER, n, p )
  {
    parse_options( options_str );
    trigger_gcd = 0_ms;
    harmful = false;
    ignore_false_positive = true;
  }

  void execute() override
  {
    mage_t* p = debug_cast<mage_t*>( player );

    p->sample_data.burn_duration_history->add( p->burn_phase.duration( sim->current_time() ).total_seconds() );

    bool success = p->burn_phase.disable( sim->current_time() );
    if ( !success )
      report_burn_switch_error( this );

    p->uptime.burn_phase->update( false, sim->current_time() );
    p->uptime.conserve_phase->update( true, sim->current_time() );
  }

  bool ready() override
  {
    if ( !debug_cast<mage_t*>( player )->burn_phase.on() )
      return false;

    return action_t::ready();
  }
};

// Proxy Freeze Action ======================================================

struct freeze_t : public action_t
{
  freeze_t( const std::string& n, mage_t* p, const std::string& options_str ) :
    action_t( ACTION_OTHER, n, p, p->find_specialization_spell( "Freeze" ) )
  {
    parse_options( options_str );
    may_miss = may_crit = callbacks = false;
    dual = usable_while_casting = ignore_false_positive = true;
    background = p->talents.lonely_winter->ok();
  }

  void execute() override
  {
    mage_t* m = debug_cast<mage_t*>( player );
    m->pets.water_elemental->action.freeze->set_target( target );
    m->pets.water_elemental->action.freeze->execute();
  }

  bool ready() override
  {
    mage_t* m = debug_cast<mage_t*>( player );

    if ( !m->pets.water_elemental || m->pets.water_elemental->is_sleeping() )
      return false;

    // Make sure the cooldown is actually ready and not just within cooldown tolerance.
    auto freeze = m->pets.water_elemental->action.freeze;
    if ( !freeze->cooldown->up() || !freeze->ready() )
      return false;

    return action_t::ready();
  }
};

}  // namespace actions

namespace events {

struct icicle_event_t : public event_t
{
  mage_t* mage;
  player_t* target;

  icicle_event_t( mage_t& m, player_t* t, bool first = false ) :
    event_t( m ),
    mage( &m ),
    target( t )
  {
    schedule( first ? 0.25_s : 0.4_s * mage->cache.spell_speed() );
  }

  const char* name() const override
  { return "icicle_event"; }

  void execute() override
  {
    mage->icicle_event = nullptr;

    // If the target of the icicle is dead, stop the chain
    if ( target->is_sleeping() )
    {
      sim().print_debug( "{} icicle use on {} (sleeping target), stopping", mage->name(), target->name() );
      return;
    }

    action_t* icicle_action = mage->get_icicle();
    if ( !icicle_action )
      return;

    icicle_action->set_target( target );
    icicle_action->execute();

    if ( !mage->icicles.empty() )
    {
      mage->icicle_event = make_event<icicle_event_t>( sim(), *mage, target );
      sim().print_debug( "{} icicle use on {} (chained), total={}", mage->name(), target->name(), mage->icicles.size() );
    }
  }
};

struct ignite_spread_event_t : public event_t
{
  mage_t* mage;

  static double ignite_bank( dot_t* ignite )
  {
    if ( !ignite->is_ticking() )
      return 0.0;

    auto ignite_state = debug_cast<residual_action::residual_periodic_state_t*>( ignite->state );
    return ignite_state->tick_amount * ignite->ticks_left();
  }

  ignite_spread_event_t( mage_t& m, timespan_t delta_time ) :
    event_t( m, delta_time ),
    mage( &m )
  { }

  const char* name() const override
  { return "ignite_spread_event"; }

  void execute() override
  {
    mage->ignite_spread_event = nullptr;
    mage->procs.ignite_spread->occur();

    sim().print_log( "{} ignite spread event occurs", mage->name() );

    const auto& tl = sim().target_non_sleeping_list;
    if ( tl.size() > 1 )
    {
      std::vector<dot_t*> active_ignites;
      std::vector<dot_t*> candidates;
      // Split ignite targets by whether ignite is ticking
      for ( auto t : tl )
      {
        if ( !t->is_enemy() )
          continue;

        dot_t* ignite = t->get_dot( "ignite", mage );
        if ( ignite->is_ticking() )
          active_ignites.push_back( ignite );
        else
          candidates.push_back( ignite );
      }

      // Sort active ignites by descending bank size
      std::stable_sort( active_ignites.begin(), active_ignites.end(), [] ( dot_t* a, dot_t* b )
      { return ignite_bank( a ) > ignite_bank( b ); } );

      // Loop over active ignites:
      // - Pop smallest ignite for spreading
      // - Remove equal sized ignites from tail of spread candidate list
      // - Choose random target and execute spread
      // - Remove spread destination from candidate list
      // - Add spreaded ignite source to candidate list
      // This algorithm provides random selection of the spread target, while
      // guaranteeing that every source will have a larger ignite bank than the
      // destination. It also guarantees that each ignite will spread to a unique
      // target. This allows us to avoid N^2 spread validity checks.
      while ( !active_ignites.empty() )
      {
        dot_t* source = active_ignites.back();
        active_ignites.pop_back();
        double source_bank = ignite_bank( source );

        if ( !candidates.empty() )
        {
          // Skip candidates that have equal ignite bank size to the source
          int index = as<int>( candidates.size() ) - 1;
          while ( index >= 0 && ignite_bank( candidates[ index ] ) == source_bank )
            index--;

          // No valid spread targets
          if ( index < 0 )
            continue;

          // TODO: Filter valid candidates by ignite spread range

          // Randomly select spread target from remaining candidates
          index = rng().range( index );
          dot_t* destination = candidates[ index ];

          if ( destination->is_ticking() )
            mage->procs.ignite_overwrite->occur();
          else
            mage->procs.ignite_new_spread->occur();

          sim().print_log( "{} ignite spreads from {} to {} ({})",
                           mage->name(), source->target->name(), destination->target->name(),
                           destination->is_ticking() ? "overwrite" : "new" );

          destination->cancel();
          source->copy( destination->target, DOT_COPY_CLONE );

          // Remove spread destination from candidates
          candidates.erase( candidates.begin() + index );
        }

        // Add spread source to candidates
        candidates.push_back( source );
      }
    }

    // Schedule next spread for 2 seconds later
    mage->ignite_spread_event = make_event<events::ignite_spread_event_t>(
      sim(), *mage, mage->spec.ignite->effectN( 3 ).period() );
  }
};

struct time_anomaly_tick_event_t : public event_t
{
  mage_t* mage;

  enum ta_proc_type_e
  {
    TA_ARCANE_POWER,
    TA_EVOCATION,
    TA_ARCANE_CHARGE
  };

  time_anomaly_tick_event_t( mage_t& m, timespan_t delta_time ) :
    event_t( m, delta_time ),
    mage( &m )
  { }

  const char* name() const override
  { return "time_anomaly_tick_event"; }

  void execute() override
  {
    mage->time_anomaly_tick_event = nullptr;
    sim().print_log( "{} Time Anomaly tick event occurs.", mage->name() );

    if ( mage->shuffled_rng.time_anomaly->trigger() )
    {
      sim().print_log( "{} Time Anomaly proc successful, triggering effects.", mage->name() );

      std::vector<ta_proc_type_e> possible_procs;

      if ( mage->buffs.arcane_power->check() == 0 )
        possible_procs.push_back( TA_ARCANE_POWER );
      if ( mage->buffs.evocation->check() == 0 )
        possible_procs.push_back( TA_EVOCATION );
      if ( mage->buffs.arcane_charge->check() < 3 )
        possible_procs.push_back( TA_ARCANE_CHARGE );

      if ( !possible_procs.empty() )
      {
        auto proc = possible_procs[ rng().range( possible_procs.size() ) ];
        switch ( proc )
        {
          case TA_ARCANE_POWER:
          {
            timespan_t duration = 1000 * mage->talents.time_anomaly->effectN( 1 ).time_value();
            mage->buffs.arcane_power->trigger( 1, buff_t::DEFAULT_VALUE(), -1.0, duration );
            break;
          }
          case TA_EVOCATION:
          {
            timespan_t duration = 1000 * mage->talents.time_anomaly->effectN( 2 ).time_value();
            mage->trigger_evocation( duration, false );
            break;
          }
          case TA_ARCANE_CHARGE:
          {
            unsigned charges = as<unsigned>( mage->talents.time_anomaly->effectN( 3 ).base_value() );
            mage->trigger_arcane_charge( charges );
            break;
          }
          default:
            break;
        }
      }
    }

    mage->time_anomaly_tick_event = make_event<events::time_anomaly_tick_event_t>(
      sim(), *mage, mage->talents.time_anomaly->effectN( 1 ).period() );
  }
};

}  // namespace events

// ==========================================================================
// Mage Character Definition
// ==========================================================================

mage_td_t::mage_td_t( player_t* target, mage_t* mage ) :
  actor_target_data_t( target, mage ),
  dots(),
  debuffs()
{
  dots.nether_tempest = target->get_dot( "nether_tempest", mage );

  debuffs.frozen            = make_buff( *this, "frozen" )
                                ->set_duration( mage->options.frozen_duration );
  debuffs.winters_chill     = make_buff( *this, "winters_chill", mage->find_spell( 228358 ) )
                                ->set_chance( mage->spec.brain_freeze_2->ok() );
  debuffs.touch_of_the_magi = make_buff<buffs::touch_of_the_magi_t>( this );
  debuffs.packed_ice        = make_buff( *this, "packed_ice", mage->find_spell( 272970 ) )
                                ->set_chance( mage->azerite.packed_ice.enabled() )
                                ->set_default_value( mage->azerite.packed_ice.value() );
}

mage_t::mage_t( sim_t* sim, const std::string& name, race_e r ) :
  player_t( sim, MAGE, name, r ),
  icicle_event(),
  icicle(),
  ignite(),
  ignite_spread_event(),
  time_anomaly_tick_event(),
  last_bomb_target(),
  last_frostbolt_target(),
  distance_from_rune(),
  lucid_dreams_refund(),
  strive_for_perfection_multiplier(),
  vision_of_perfection_multiplier(),
  action(),
  benefits(),
  buffs(),
  cooldowns(),
  gains(),
  options(),
  pets(),
  procs(),
  shuffled_rng(),
  sample_data(),
  spec(),
  state(),
  talents(),
  azerite(),
  uptime()
{
  // Cooldowns
  cooldowns.combustion       = get_cooldown( "combustion"       );
  cooldowns.cone_of_cold     = get_cooldown( "cone_of_cold"     );
  cooldowns.fire_blast       = get_cooldown( "fire_blast"       );
  cooldowns.frost_nova       = get_cooldown( "frost_nova"       );
  cooldowns.frozen_orb       = get_cooldown( "frozen_orb"       );
  cooldowns.presence_of_mind = get_cooldown( "presence_of_mind" );

  // Options
  regen_type = REGEN_DYNAMIC;
}

bool mage_t::trigger_crowd_control( const action_state_t* s, spell_mechanic type )
{
  if ( type == MECHANIC_INTERRUPT )
    return true;

  if ( action_t::result_is_hit( s->result )
    && ( s->target->is_add() || s->target->level() < sim->max_player_level + 3 ) )
  {
    if ( type == MECHANIC_ROOT )
      get_target_data( s->target )->debuffs.frozen->trigger();

    return true;
  }

  return false;
}

action_t* mage_t::create_action( const std::string& name, const std::string& options_str )
{
  using namespace actions;

  // Arcane
  if ( name == "arcane_barrage"         ) return new         arcane_barrage_t( name, this, options_str );
  if ( name == "arcane_blast"           ) return new           arcane_blast_t( name, this, options_str );
  if ( name == "arcane_explosion"       ) return new       arcane_explosion_t( name, this, options_str );
  if ( name == "arcane_familiar"        ) return new        arcane_familiar_t( name, this, options_str );
  if ( name == "arcane_missiles"        ) return new        arcane_missiles_t( name, this, options_str );
  if ( name == "arcane_orb"             ) return new             arcane_orb_t( name, this, options_str );
  if ( name == "arcane_power"           ) return new           arcane_power_t( name, this, options_str );
  if ( name == "charged_up"             ) return new             charged_up_t( name, this, options_str );
  if ( name == "evocation"              ) return new              evocation_t( name, this, options_str );
  if ( name == "nether_tempest"         ) return new         nether_tempest_t( name, this, options_str );
  if ( name == "presence_of_mind"       ) return new       presence_of_mind_t( name, this, options_str );
  if ( name == "slow"                   ) return new                   slow_t( name, this, options_str );
  if ( name == "supernova"              ) return new              supernova_t( name, this, options_str );

  if ( name == "start_burn_phase"       ) return new       start_burn_phase_t( name, this, options_str );
  if ( name == "stop_burn_phase"        ) return new        stop_burn_phase_t( name, this, options_str );

  // Fire
  if ( name == "blast_wave"             ) return new             blast_wave_t( name, this, options_str );
  if ( name == "combustion"             ) return new             combustion_t( name, this, options_str );
  if ( name == "dragons_breath"         ) return new         dragons_breath_t( name, this, options_str );
  if ( name == "fire_blast"             ) return new             fire_blast_t( name, this, options_str );
  if ( name == "fireball"               ) return new               fireball_t( name, this, options_str );
  if ( name == "flamestrike"            ) return new            flamestrike_t( name, this, options_str );
  if ( name == "living_bomb"            ) return new            living_bomb_t( name, this, options_str );
  if ( name == "meteor"                 ) return new                 meteor_t( name, this, options_str );
  if ( name == "phoenix_flames"         ) return new         phoenix_flames_t( name, this, options_str );
  if ( name == "pyroblast"              ) return new              pyroblast_t( name, this, options_str );
  if ( name == "scorch"                 ) return new                 scorch_t( name, this, options_str );

  // Frost
  if ( name == "blizzard"               ) return new               blizzard_t( name, this, options_str );
  if ( name == "cold_snap"              ) return new              cold_snap_t( name, this, options_str );
  if ( name == "comet_storm"            ) return new            comet_storm_t( name, this, options_str );
  if ( name == "cone_of_cold"           ) return new           cone_of_cold_t( name, this, options_str );
  if ( name == "ebonbolt"               ) return new               ebonbolt_t( name, this, options_str );
  if ( name == "flurry"                 ) return new                 flurry_t( name, this, options_str );
  if ( name == "frostbolt"              ) return new              frostbolt_t( name, this, options_str );
  if ( name == "frozen_orb"             ) return new             frozen_orb_t( name, this, options_str );
  if ( name == "glacial_spike"          ) return new          glacial_spike_t( name, this, options_str );
  if ( name == "ice_floes"              ) return new              ice_floes_t( name, this, options_str );
  if ( name == "ice_lance"              ) return new              ice_lance_t( name, this, options_str );
  if ( name == "ice_nova"               ) return new               ice_nova_t( name, this, options_str );
  if ( name == "icy_veins"              ) return new              icy_veins_t( name, this, options_str );
  if ( name == "ray_of_frost"           ) return new           ray_of_frost_t( name, this, options_str );
  if ( name == "summon_water_elemental" ) return new summon_water_elemental_t( name, this, options_str );

  if ( name == "freeze"                 ) return new                 freeze_t( name, this, options_str );

  // Shared spells
  if ( name == "arcane_intellect"       ) return new       arcane_intellect_t( name, this, options_str );
  if ( name == "blink"                  ) return new                  blink_t( name, this, options_str );
  if ( name == "counterspell"           ) return new           counterspell_t( name, this, options_str );
  if ( name == "frost_nova"             ) return new             frost_nova_t( name, this, options_str );
  if ( name == "time_warp"              ) return new              time_warp_t( name, this, options_str );

  // Shared talents
  if ( name == "mirror_image"           ) return new           mirror_image_t( name, this, options_str );
  if ( name == "rune_of_power"          ) return new          rune_of_power_t( name, this, options_str );
  if ( name == "shimmer"                ) return new                shimmer_t( name, this, options_str );

  // Special
  if ( name == "blink_any" )
    return create_action( talents.shimmer->ok() ? "shimmer" : "blink", options_str );

  return player_t::create_action( name, options_str );
}

void mage_t::create_actions()
{
  using namespace actions;

  if ( spec.ignite->ok() )
    ignite = get_action<ignite_t>( "ignite", this );

  if ( spec.icicles->ok() )
  {
    icicle.frostbolt    = get_action<icicle_t>( "frostbolt_icicle", this );
    icicle.flurry       = get_action<icicle_t>( "flurry_icicle", this );
    icicle.lucid_dreams = get_action<icicle_t>( "lucid_dreams_icicle", this );
  }

  if ( talents.arcane_familiar->ok() )
    action.arcane_assault = get_action<arcane_assault_t>( "arcane_assault", this );

  if ( talents.conflagration->ok() )
    action.conflagration_flare_up = get_action<conflagration_flare_up_t>( "conflagration_flare_up", this );

  if ( talents.living_bomb->ok() )
  {
    action.living_bomb_dot        = get_action<living_bomb_dot_t>( "living_bomb_dot", this, true );
    action.living_bomb_dot_spread = get_action<living_bomb_dot_t>( "living_bomb_dot_spread", this, false );
    action.living_bomb_explosion  = get_action<living_bomb_explosion_t>( "living_bomb_explosion", this );
  }

  if ( talents.meteor->ok() )
  {
    action.meteor_burn   = get_action<meteor_burn_t>( "meteor_burn", this );
    action.meteor_impact = get_action<meteor_impact_t>( "meteor_impact", this );
  }

  if ( talents.touch_of_the_magi->ok() )
    action.touch_of_the_magi = get_action<touch_of_the_magi_t>( "touch_of_the_magi", this );

  if ( azerite.glacial_assault.enabled() )
    action.glacial_assault = get_action<glacial_assault_t>( "glacial_assault", this );

  player_t::create_actions();
}

void mage_t::create_options()
{
  add_option( opt_timespan( "firestarter_time", options.firestarter_time ) );
  add_option( opt_timespan( "frozen_duration", options.frozen_duration ) );
  add_option( opt_timespan( "scorch_delay", options.scorch_delay ) );
  add_option( opt_int( "greater_blessing_of_wisdom_count", options.gbow_count ) );
  add_option( opt_bool( "allow_shimmer_lance", options.allow_shimmer_lance ) );
  add_option( opt_func( "rotation", [ this ] ( sim_t*, const std::string&, const std::string& val )
  {
    if ( util::str_compare_ci( val, "standard" ) )
      options.rotation = ROTATION_STANDARD;
    else if ( util::str_compare_ci( val, "no_ice_lance" ) )
      options.rotation = ROTATION_NO_ICE_LANCE;
    else if ( util::str_compare_ci( val, "frozen_orb" ) )
      options.rotation = ROTATION_FROZEN_ORB;
    else
      return false;
    return true;
  } ) );
  add_option( opt_float( "lucid_dreams_proc_chance_arcane", options.lucid_dreams_proc_chance_arcane ) );
  add_option( opt_float( "lucid_dreams_proc_chance_fire", options.lucid_dreams_proc_chance_fire ) );
  add_option( opt_float( "lucid_dreams_proc_chance_frost", options.lucid_dreams_proc_chance_frost ) );
  player_t::create_options();
}

std::string mage_t::create_profile( save_e save_type )
{
  std::string profile = player_t::create_profile( save_type );

  if ( save_type & SAVE_PLAYER )
  {
    if ( options.firestarter_time > 0_ms )
      profile += "firestarter_time=" + util::to_string( options.firestarter_time.total_seconds() ) + "\n";
    if ( options.rotation == ROTATION_NO_ICE_LANCE )
      profile += "rotation=no_ice_lance\n";
    if ( options.rotation == ROTATION_FROZEN_ORB )
      profile += "rotation=frozen_orb\n";
  }

  return profile;
}

void mage_t::copy_from( player_t* source )
{
  player_t::copy_from( source );
  options = debug_cast<mage_t*>( source )->options;
}

void mage_t::merge( player_t& other )
{
  player_t::merge( other );

  mage_t& mage = dynamic_cast<mage_t&>( other );

  for ( size_t i = 0; i < cooldown_waste_data_list.size(); i++ )
    cooldown_waste_data_list[ i ]->merge( *mage.cooldown_waste_data_list[ i ] );

  for ( size_t i = 0; i < shatter_source_list.size(); i++ )
    shatter_source_list[ i ]->merge( *mage.shatter_source_list[ i ] );

  switch ( specialization() )
  {
    case MAGE_ARCANE:
      sample_data.burn_duration_history->merge( *mage.sample_data.burn_duration_history );
      sample_data.burn_initial_mana->merge( *mage.sample_data.burn_initial_mana );
      break;
    case MAGE_FROST:
      if ( talents.thermal_void->ok() )
        sample_data.icy_veins_duration->merge( *mage.sample_data.icy_veins_duration );
      break;
    default:
      break;
  }
}

void mage_t::analyze( sim_t& s )
{
  player_t::analyze( s );

  range::for_each( cooldown_waste_data_list, std::mem_fn( &cooldown_waste_data_t::analyze ) );

  switch ( specialization() )
  {
    case MAGE_ARCANE:
      sample_data.burn_duration_history->analyze();
      sample_data.burn_initial_mana->analyze();
      break;
    case MAGE_FROST:
      if ( talents.thermal_void->ok() )
        sample_data.icy_veins_duration->analyze();
      break;
    default:
      break;
  }
}

void mage_t::datacollection_begin()
{
  player_t::datacollection_begin();

  range::for_each( cooldown_waste_data_list, std::mem_fn( &cooldown_waste_data_t::datacollection_begin ) );
  range::for_each( shatter_source_list, std::mem_fn( &shatter_source_t::datacollection_begin ) );
}

void mage_t::datacollection_end()
{
  player_t::datacollection_end();

  range::for_each( cooldown_waste_data_list, std::mem_fn( &cooldown_waste_data_t::datacollection_end ) );
  range::for_each( shatter_source_list, std::mem_fn( &shatter_source_t::datacollection_end ) );
}

void mage_t::regen( timespan_t periodicity )
{
  player_t::regen( periodicity );

  if ( resources.is_active( RESOURCE_MANA ) && buffs.evocation->check() )
  {
    double base = resource_regen_per_second( RESOURCE_MANA );
    if ( base )
    {
      // Base regen was already done, subtract 1.0 from Evocation's mana regen multiplier to make
      // sure we don't apply it twice.
      resource_gain(
        RESOURCE_MANA,
        ( buffs.evocation->check_value() - 1.0 ) * base * periodicity.total_seconds(),
        gains.evocation );
    }
  }
}

void mage_t::moving()
{
  if ( ( executing  && !executing->usable_moving() )
    || ( queueing   && !queueing->usable_moving() )
    || ( channeling && !channeling->usable_moving() ) )
  {
    player_t::moving();
  }
}

void mage_t::create_pets()
{
  if ( specialization() == MAGE_FROST && !talents.lonely_winter->ok() && find_action( "summon_water_elemental" ) )
    pets.water_elemental = new pets::water_elemental::water_elemental_pet_t( sim, this );

  if ( talents.mirror_image->ok() && find_action( "mirror_image" ) )
  {
    for ( int i = 0; i < as<int>( talents.mirror_image->effectN( 2 ).base_value() ); i++ )
    {
      auto image = new pets::mirror_image::mirror_image_pet_t( sim, this );
      if ( i > 0 )
        image->quiet = true;
      pets.mirror_images.push_back( image );
    }
  }
}

void mage_t::init_spells()
{
  player_t::init_spells();

  // Talents
  // Tier 15
  talents.amplification      = find_talent_spell( "Amplification"      );
  talents.rule_of_threes     = find_talent_spell( "Rule of Threes"     );
  talents.arcane_familiar    = find_talent_spell( "Arcane Familiar"    );
  talents.firestarter        = find_talent_spell( "Firestarter"        );
  talents.pyromaniac         = find_talent_spell( "Pyromaniac"         );
  talents.searing_touch      = find_talent_spell( "Searing Touch"      );
  talents.bone_chilling      = find_talent_spell( "Bone Chilling"      );
  talents.lonely_winter      = find_talent_spell( "Lonely Winter"      );
  talents.ice_nova           = find_talent_spell( "Ice Nova"           );
  // Tier 30
  talents.shimmer            = find_talent_spell( "Shimmer"            );
  talents.mana_shield        = find_talent_spell( "Mana Shield"        );
  talents.slipstream         = find_talent_spell( "Slipstream"         );
  talents.blazing_soul       = find_talent_spell( "Blazing Soul"       );
  talents.blast_wave         = find_talent_spell( "Blast Wave"         );
  talents.glacial_insulation = find_talent_spell( "Glacial Insulation" );
  talents.ice_floes          = find_talent_spell( "Ice Floes"          );
  // Tier 45
  talents.incanters_flow     = find_talent_spell( "Incanter's Flow"    );
  talents.mirror_image       = find_talent_spell( "Mirror Image"       );
  talents.rune_of_power      = find_talent_spell( "Rune of Power"      );
  // Tier 60
  talents.resonance          = find_talent_spell( "Resonance"          );
  talents.charged_up         = find_talent_spell( "Charged Up"         );
  talents.supernova          = find_talent_spell( "Supernova"          );
  talents.flame_on           = find_talent_spell( "Flame On"           );
  talents.alexstraszas_fury  = find_talent_spell( "Alexstrasza's Fury" );
  talents.phoenix_flames     = find_talent_spell( "Phoenix Flames"     );
  talents.frozen_touch       = find_talent_spell( "Frozen Touch"       );
  talents.chain_reaction     = find_talent_spell( "Chain Reaction"     );
  talents.ebonbolt           = find_talent_spell( "Ebonbolt"           );
  // Tier 75
  talents.ice_ward           = find_talent_spell( "Ice Ward"           );
  talents.ring_of_frost      = find_talent_spell( "Ring of Frost"      );
  talents.chrono_shift       = find_talent_spell( "Chrono Shift"       );
  talents.frenetic_speed     = find_talent_spell( "Frenetic Speed"     );
  talents.frigid_winds       = find_talent_spell( "Frigid Winds"       );
  // Tier 90
  talents.reverberate        = find_talent_spell( "Reverberate"        );
  talents.touch_of_the_magi  = find_talent_spell( "Touch of the Magi"  );
  talents.nether_tempest     = find_talent_spell( "Nether Tempest"     );
  talents.flame_patch        = find_talent_spell( "Flame Patch"        );
  talents.conflagration      = find_talent_spell( "Conflagration"      );
  talents.living_bomb        = find_talent_spell( "Living Bomb"        );
  talents.freezing_rain      = find_talent_spell( "Freezing Rain"      );
  talents.splitting_ice      = find_talent_spell( "Splitting Ice"      );
  talents.comet_storm        = find_talent_spell( "Comet Storm"        );
  // Tier 100
  talents.overpowered        = find_talent_spell( "Overpowered"        );
  talents.time_anomaly       = find_talent_spell( "Time Anomaly"       );
  talents.arcane_orb         = find_talent_spell( "Arcane Orb"         );
  talents.kindling           = find_talent_spell( "Kindling"           );
  talents.pyroclasm          = find_talent_spell( "Pyroclasm"          );
  talents.meteor             = find_talent_spell( "Meteor"             );
  talents.thermal_void       = find_talent_spell( "Thermal Void"       );
  talents.ray_of_frost       = find_talent_spell( "Ray of Frost"       );
  talents.glacial_spike      = find_talent_spell( "Glacial Spike"      );

  // Spec Spells
  spec.arcane_barrage_2      = find_specialization_spell( 231564 );
  spec.arcane_charge         = find_spell( 36032 );
  spec.arcane_mage           = find_specialization_spell( 137021 );
  spec.clearcasting          = find_specialization_spell( "Clearcasting" );
  spec.evocation_2           = find_specialization_spell( 231565 );

  spec.critical_mass         = find_specialization_spell( "Critical Mass" );
  spec.critical_mass_2       = find_specialization_spell( 231630 );
  spec.enhanced_pyrotechnics = find_specialization_spell( 157642 );
  spec.fire_blast_2          = find_specialization_spell( 231568 );
  spec.fire_blast_3          = find_specialization_spell( 231567 );
  spec.fire_mage             = find_specialization_spell( 137019 );
  spec.hot_streak            = find_specialization_spell( 195283 );

  spec.brain_freeze          = find_specialization_spell( "Brain Freeze" );
  spec.brain_freeze_2        = find_specialization_spell( 231584 );
  spec.blizzard_2            = find_specialization_spell( 236662 );
  spec.fingers_of_frost      = find_specialization_spell( "Fingers of Frost" );
  spec.frost_mage            = find_specialization_spell( 137020 );
  spec.shatter               = find_specialization_spell( "Shatter" );
  spec.shatter_2             = find_specialization_spell( 231582 );


  // Mastery
  spec.savant                = find_mastery_spell( MAGE_ARCANE );
  spec.ignite                = find_mastery_spell( MAGE_FIRE );
  spec.icicles               = find_mastery_spell( MAGE_FROST );

  // Azerite
  azerite.arcane_pressure          = find_azerite_spell( "Arcane Pressure"          );
  azerite.arcane_pummeling         = find_azerite_spell( "Arcane Pummeling"         );
  azerite.brain_storm              = find_azerite_spell( "Brain Storm"              );
  azerite.equipoise                = find_azerite_spell( "Equipoise"                );
  azerite.explosive_echo           = find_azerite_spell( "Explosive Echo"           );
  azerite.galvanizing_spark        = find_azerite_spell( "Galvanizing Spark"        );

  azerite.blaster_master           = find_azerite_spell( "Blaster Master"           );
  azerite.duplicative_incineration = find_azerite_spell( "Duplicative Incineration" );
  azerite.firemind                 = find_azerite_spell( "Firemind"                 );
  azerite.flames_of_alacrity       = find_azerite_spell( "Flames of Alacrity"       );
  azerite.trailing_embers          = find_azerite_spell( "Trailing Embers"          );
  azerite.wildfire                 = find_azerite_spell( "Wildfire"                 );

  azerite.flash_freeze             = find_azerite_spell( "Flash Freeze"             );
  azerite.frigid_grasp             = find_azerite_spell( "Frigid Grasp"             );
  azerite.glacial_assault          = find_azerite_spell( "Glacial Assault"          );
  azerite.packed_ice               = find_azerite_spell( "Packed Ice"               );
  azerite.tunnel_of_ice            = find_azerite_spell( "Tunnel of Ice"            );
  azerite.whiteout                 = find_azerite_spell( "Whiteout"                 );

  auto memory = find_azerite_essence( "Memory of Lucid Dreams" );
  lucid_dreams_refund = memory.spell( 1u, essence_type::MINOR )->effectN( 1 ).percent();

  auto vision = find_azerite_essence( "Vision of Perfection" );
  strive_for_perfection_multiplier = 1.0 + azerite::vision_of_perfection_cdr( vision );
  vision_of_perfection_multiplier =
    vision.spell( 1u, essence_type::MAJOR )->effectN( 1 ).percent() +
    vision.spell( 2u, essence_spell::UPGRADE, essence_type::MAJOR )->effectN( 1 ).percent();
}

void mage_t::init_base_stats()
{
  if ( base.distance < 1 )
    base.distance = 30;

  player_t::init_base_stats();

  base.spell_power_per_intellect = 1.0;

  // Mana Attunement
  resources.base_regen_per_second[ RESOURCE_MANA ] *= 1.0 + find_spell( 121039 )->effectN( 1 ).percent();

  if ( specialization() == MAGE_ARCANE )
    regen_caches[ CACHE_MASTERY ] = true;
}

void mage_t::create_buffs()
{
  player_t::create_buffs();

  // Arcane
  buffs.arcane_charge        = make_buff( this, "arcane_charge", spec.arcane_charge );
  buffs.arcane_power         = make_buff( this, "arcane_power", find_spell( 12042 ) )
                                 ->set_cooldown( 0_ms )
                                 ->set_default_value( find_spell( 12042 )->effectN( 1 ).percent()
                                                    + talents.overpowered->effectN( 1 ).percent() );
  buffs.clearcasting         = make_buff( this, "clearcasting", find_spell( 263725 ) )
                                 ->set_default_value( find_spell( 263725 )->effectN( 1 ).percent() );
  buffs.clearcasting_channel = make_buff( this, "clearcasting_channel", find_spell( 277726 ) )
                                 ->set_quiet( true );
  buffs.evocation            = make_buff( this, "evocation", find_spell( 12051 ) )
                                 ->set_default_value( find_spell( 12051 )->effectN( 1 ).percent() )
                                 ->set_cooldown( 0_ms )
                                 ->set_affects_regen( true );
  buffs.presence_of_mind     = make_buff( this, "presence_of_mind", find_spell( 205025 ) )
                                 ->set_cooldown( 0_ms )
                                 ->set_stack_change_callback( [ this ] ( buff_t*, int, int cur )
                                   { if ( cur == 0 ) cooldowns.presence_of_mind->start(); } );

  buffs.arcane_familiar      = make_buff( this, "arcane_familiar", find_spell( 210126 ) )
                                 ->set_default_value( find_spell( 210126 )->effectN( 1 ).percent() )
                                 ->set_period( 3.0_s )
                                 ->set_tick_time_behavior( buff_tick_time_behavior::HASTED )
                                 ->set_tick_callback( [ this ] ( buff_t*, int, const timespan_t& )
                                   {
                                     action.arcane_assault->set_target( target );
                                     action.arcane_assault->execute();
                                   } )
                                 ->set_stack_change_callback( [ this ] ( buff_t*, int, int )
                                   { recalculate_resource_max( RESOURCE_MANA ); } );
  buffs.chrono_shift         = make_buff( this, "chrono_shift", find_spell( 236298 ) )
                                 ->set_default_value( find_spell( 236298 )->effectN( 1 ).percent() )
                                 ->add_invalidate( CACHE_RUN_SPEED )
                                 ->set_chance( talents.chrono_shift->ok() );
  buffs.rule_of_threes       = make_buff( this, "rule_of_threes", find_spell( 264774 ) )
                                 ->set_default_value( find_spell( 264774 )->effectN( 1 ).percent() )
                                 ->set_chance( talents.rule_of_threes->ok() );


  // Fire
  buffs.combustion            = make_buff<buffs::combustion_buff_t>( this );
  buffs.enhanced_pyrotechnics = make_buff( this, "enhanced_pyrotechnics", find_spell( 157644 ) )
                                  ->set_chance( spec.enhanced_pyrotechnics->ok() )
                                  ->set_default_value( find_spell( 157644 )->effectN( 1 ).percent() )
                                  ->set_stack_change_callback( [ this ] ( buff_t*, int old, int cur )
                                    {
                                      if ( cur > old )
                                        buffs.flames_of_alacrity->trigger( cur - old );
                                      else
                                        buffs.flames_of_alacrity->decrement( old - cur );
                                    } );
  buffs.heating_up            = make_buff( this, "heating_up", find_spell( 48107 ) );
  buffs.hot_streak            = make_buff( this, "hot_streak", find_spell( 48108 ) );

  buffs.frenetic_speed        = make_buff( this, "frenetic_speed", find_spell( 236060 ) )
                                  ->set_default_value( find_spell( 236060 )->effectN( 1 ).percent() )
                                  ->add_invalidate( CACHE_RUN_SPEED )
                                  ->set_chance( talents.frenetic_speed->ok() );
  buffs.pyroclasm             = make_buff( this, "pyroclasm", find_spell( 269651 ) )
                                  ->set_default_value( find_spell( 269651 )->effectN( 1 ).percent() )
                                  ->set_chance( talents.pyroclasm->effectN( 1 ).percent() );


  // Frost
  buffs.brain_freeze     = make_buff( this, "brain_freeze", find_spell( 190446 ) );
  buffs.fingers_of_frost = make_buff( this, "fingers_of_frost", find_spell( 44544 ) );
  buffs.icicles          = make_buff( this, "icicles", find_spell( 205473 ) );
  buffs.icy_veins        = make_buff<buffs::icy_veins_buff_t>( this );

  buffs.bone_chilling    = make_buff( this, "bone_chilling", find_spell( 205766 ) )
                             ->set_default_value( 0.1 * talents.bone_chilling->effectN( 1 ).percent() )
                             ->set_chance( talents.bone_chilling->ok() );
  buffs.chain_reaction   = make_buff( this, "chain_reaction", find_spell( 278310 ) )
                             ->set_default_value( find_spell( 278310 )->effectN( 1 ).percent() )
                             ->set_chance( talents.chain_reaction->ok() );
  buffs.freezing_rain    = make_buff( this, "freezing_rain", find_spell( 270232 ) )
                             ->set_default_value( find_spell( 270232 )->effectN( 2 ).percent() )
                             ->set_chance( talents.freezing_rain->ok() );
  buffs.ice_floes        = make_buff<buffs::ice_floes_buff_t>( this );
  buffs.ray_of_frost     = make_buff( this, "ray_of_frost", find_spell( 208141 ) )
                             ->set_default_value( find_spell( 208141 )->effectN( 1 ).percent() );


  // Shared
  buffs.incanters_flow = make_buff<buffs::incanters_flow_t>( this );
  buffs.rune_of_power  = make_buff( this, "rune_of_power", find_spell( 116014 ) )
                           ->set_default_value( find_spell( 116014 )->effectN( 1 ).percent() );

  // Azerite
  buffs.arcane_pummeling   = make_buff( this, "arcane_pummeling", find_spell( 270670 ) )
                               ->set_default_value( azerite.arcane_pummeling.value() )
                               ->set_chance( azerite.arcane_pummeling.enabled() );
  buffs.brain_storm        = make_buff<stat_buff_t>( this, "brain_storm", find_spell( 273330 ) )
                               ->add_stat( STAT_INTELLECT, azerite.brain_storm.value() )
                               ->set_chance( azerite.brain_storm.enabled() );

  buffs.blaster_master     = make_buff<stat_buff_t>( this, "blaster_master", find_spell( 274598 ) )
                               ->add_stat( STAT_MASTERY_RATING, azerite.blaster_master.value() )
                               ->set_chance( azerite.blaster_master.enabled() );
  buffs.firemind           = make_buff<stat_buff_t>( this, "firemind", find_spell( 279715 ) )
                               ->add_stat( STAT_INTELLECT, azerite.firemind.value() )
                               ->set_chance( azerite.firemind.enabled() );
  buffs.flames_of_alacrity = make_buff<stat_buff_t>( this, "flames_of_alacrity", find_spell( 272934 ) )
                               ->add_stat( STAT_HASTE_RATING, azerite.flames_of_alacrity.value() )
                               ->set_chance( azerite.flames_of_alacrity.enabled() );
  buffs.wildfire           = make_buff<stat_buff_t>( this, "wildfire", find_spell( 288800 ) )
                               ->set_chance( azerite.wildfire.enabled() );

  proc_t* proc_fof = get_proc( "Fingers of Frost from Frigid Grasp" );
  buffs.frigid_grasp       = make_buff<stat_buff_t>( this, "frigid_grasp", find_spell( 279684 ) )
                               ->add_stat( STAT_INTELLECT, azerite.frigid_grasp.value() )
                               ->set_stack_change_callback( [ this, proc_fof ] ( buff_t*, int old, int )
                                 { if ( old == 0 ) trigger_fof( 1.0, 1, proc_fof ); } )
                               ->set_chance( azerite.frigid_grasp.enabled() );

  buffs.tunnel_of_ice      = make_buff( this, "tunnel_of_ice", find_spell( 277904 ) )
                               ->set_default_value( azerite.tunnel_of_ice.value() )
                               ->set_chance( azerite.tunnel_of_ice.enabled() );

  // Misc
  // N active GBoWs are modeled by a single buff that gives N times as much mana.
  buffs.gbow    = make_buff( this, "greater_blessing_of_wisdom", find_spell( 203539 ) )
    ->set_tick_callback( [ this ] ( buff_t*, int, const timespan_t& )
      { resource_gain( RESOURCE_MANA, resources.max[ RESOURCE_MANA ] * 0.002 * options.gbow_count, gains.gbow ); } )
    ->set_period( 2.0_s )
    ->set_chance( options.gbow_count > 0 );
  buffs.shimmer = make_buff( this, "shimmer", find_spell( 212653 ) );

  switch ( specialization() )
  {
    case MAGE_ARCANE:
      player_t::buffs.memory_of_lucid_dreams->set_affects_regen( true );
      break;
    case MAGE_FIRE:
      player_t::buffs.memory_of_lucid_dreams->set_stack_change_callback( [ this ] ( buff_t*, int, int )
      { cooldowns.fire_blast->adjust_recharge_multiplier(); } );
      break;
    default:
      break;
  }
}

void mage_t::init_gains()
{
  player_t::init_gains();

  gains.evocation    = get_gain( "Evocation"                  );
  gains.gbow         = get_gain( "Greater Blessing of Wisdom" );
  gains.lucid_dreams = get_gain( "Lucid Dreams"               );
}

void mage_t::init_procs()
{
  player_t::init_procs();

  switch ( specialization() )
  {
    case MAGE_FIRE:
      procs.heating_up_generated         = get_proc( "Heating Up generated" );
      procs.heating_up_removed           = get_proc( "Heating Up removed" );
      procs.heating_up_ib_converted      = get_proc( "Heating Up converted with Fire Blast" );
      procs.hot_streak                   = get_proc( "Hot Streak procs" );
      procs.hot_streak_pyromaniac        = get_proc( "Hot Streak procs from Pyromaniac" );
      procs.hot_streak_spell             = get_proc( "Hot Streak spells used" );
      procs.hot_streak_spell_crit        = get_proc( "Hot Streak spell crits" );
      procs.hot_streak_spell_crit_wasted = get_proc( "Hot Streak spell crits wasted" );

      procs.ignite_applied    = get_proc( "Direct Ignite applications" );
      procs.ignite_spread     = get_proc( "Ignites spread" );
      procs.ignite_new_spread = get_proc( "Ignites spread to new targets" );
      procs.ignite_overwrite  = get_proc( "Ignites spread to targets with existing Ignite" );
      break;
    case MAGE_FROST:
      procs.brain_freeze            = get_proc( "Brain Freeze" );
      procs.brain_freeze_used       = get_proc( "Brain Freeze used" );
      procs.fingers_of_frost        = get_proc( "Fingers of Frost" );
      procs.fingers_of_frost_wasted = get_proc( "Fingers of Frost wasted due to Winter's Chill" );
      break;
    default:
      break;
  }
}

void mage_t::init_resources( bool force )
{
  player_t::init_resources( force );

  // This is the call needed to set max mana at the beginning of the sim.
  // If this is called without recalculating max mana afterwards, it will
  // overwrite the recalculating done earlier in cache_invalidate() back
  // to default max mana.
  if ( spec.savant->ok() )
    recalculate_resource_max( RESOURCE_MANA );
}

void mage_t::init_benefits()
{
  player_t::init_benefits();

  switch ( specialization() )
  {
    case MAGE_ARCANE:
      benefits.arcane_charge.arcane_barrage = std::make_unique<buff_stack_benefit_t>( buffs.arcane_charge, "Arcane Barrage" );
      benefits.arcane_charge.arcane_blast   = std::make_unique<buff_stack_benefit_t>( buffs.arcane_charge, "Arcane Blast" );
      if ( talents.nether_tempest->ok() )
        benefits.arcane_charge.nether_tempest = std::make_unique<buff_stack_benefit_t>( buffs.arcane_charge, "Nether Tempest" );
      break;
    case MAGE_FIRE:
      if ( azerite.blaster_master.enabled() )
      {
        benefits.blaster_master.combustion = std::make_unique<buff_stack_benefit_t>( buffs.blaster_master, "Combustion" );
        if ( talents.rune_of_power->ok() )
          benefits.blaster_master.rune_of_power = std::make_unique<buff_stack_benefit_t>( buffs.blaster_master, "Rune of Power" );
        if ( talents.searing_touch->ok() )
          benefits.blaster_master.searing_touch = std::make_unique<buff_stack_benefit_t>( buffs.blaster_master, "Searing Touch" );
      }
      break;
    default:
      break;
  }
}

void mage_t::init_uptimes()
{
  player_t::init_uptimes();

  switch ( specialization() )
  {
    case MAGE_ARCANE:
      uptime.burn_phase     = get_uptime( "Burn Phase" );
      uptime.conserve_phase = get_uptime( "Conserve Phase" );

      sample_data.burn_duration_history = std::make_unique<extended_sample_data_t>( "Burn duration history", false );
      sample_data.burn_initial_mana     = std::make_unique<extended_sample_data_t>( "Burn initial mana", false );
      break;
    case MAGE_FROST:
      sample_data.blizzard = std::make_unique<cooldown_reduction_data_t>( cooldowns.frozen_orb, "Blizzard" );

      if ( talents.thermal_void->ok() )
        sample_data.icy_veins_duration = std::make_unique<extended_sample_data_t>( "Icy Veins duration", false );
      break;
    default:
      break;
  }
}

void mage_t::init_rng()
{
  player_t::init_rng();

  // TODO: There's no data about this in game. Keep an eye out in case Blizzard
  // changes this behind the scenes.
  shuffled_rng.time_anomaly = get_shuffled_rng( "time_anomaly", 1, 16 );
}

void mage_t::init_assessors()
{
  player_t::init_assessors();

  if ( talents.touch_of_the_magi->ok() )
  {
    auto assessor_fn = [ this ] ( dmg_e, action_state_t* s )
    {
      if ( auto td = target_data[ s->target ] )
      {
        auto buff = debug_cast<buffs::touch_of_the_magi_t*>( td->debuffs.touch_of_the_magi );
        if ( buff->check() )
          buff->accumulate_damage( s );
      }

      return assessor::CONTINUE;
    };

    assessor_out_damage.add( assessor::TARGET_DAMAGE - 1, assessor_fn );
    for ( auto pet : pet_list )
      pet->assessor_out_damage.add( assessor::TARGET_DAMAGE - 1, assessor_fn );
  }
}

void mage_t::init_finished()
{
  player_t::init_finished();

  // Sort the procs to put the proc sources next to each other.
  if ( specialization() == MAGE_FROST )
    range::sort( proc_list, [] ( proc_t* a, proc_t* b ) { return a->name_str < b->name_str; } );
}

void mage_t::init_action_list()
{
  if ( action_list_str.empty() )
  {
    clear_action_priority_lists();

    apl_precombat();
    switch ( specialization() )
    {
      case MAGE_ARCANE:
        apl_arcane();
        break;
      case MAGE_FIRE:
        apl_fire();
        break;
      case MAGE_FROST:
        apl_frost();
        break;
      default:
        break;
    }

    use_default_action_list = true;
  }

  player_t::init_action_list();
}

void mage_t::apl_precombat()
{
  action_priority_list_t* precombat = get_action_priority_list( "precombat" );

  precombat->add_action( "flask" );
  precombat->add_action( "food" );
  precombat->add_action( "augmentation" );
  precombat->add_action( this, "Arcane Intellect" );

  switch ( specialization() )
  {
    case MAGE_ARCANE:
      precombat->add_talent( this, "Arcane Familiar" );
      precombat->add_action( "variable,name=conserve_mana,op=set,value=60+20*azerite.equipoise.enabled",
        "conserve_mana is the mana percentage we want to go down to during conserve. It needs to leave enough room to worst case scenario spam AB only during AP." );
      break;
    case MAGE_FIRE:
      precombat->add_action( "variable,name=combustion_rop_cutoff,op=set,value=60",
        "This variable sets the time at which Rune of Power should start being saved for the next Combustion phase" );
      precombat->add_action( "variable,name=combustion_on_use,op=set,value=equipped.notorious_aspirants_badge|equipped.notorious_gladiators_badge|equipped.sinister_gladiators_badge|equipped.sinister_aspirants_badge|equipped.dread_gladiators_badge|equipped.dread_aspirants_badge|equipped.dread_combatants_insignia|equipped.notorious_aspirants_medallion|equipped.notorious_gladiators_medallion|equipped.sinister_gladiators_medallion|equipped.sinister_aspirants_medallion|equipped.dread_gladiators_medallion|equipped.dread_aspirants_medallion|equipped.dread_combatants_medallion|equipped.ignition_mages_fuse|equipped.tzanes_barkspines|equipped.azurethos_singed_plumage|equipped.ancient_knot_of_wisdom|equipped.shockbiters_fang|equipped.neural_synapse_enhancer|equipped.balefire_branch" );
      precombat->add_action( "variable,name=font_double_on_use,op=set,value=equipped.azsharas_font_of_power&variable.combustion_on_use" );
      precombat->add_action( "variable,name=on_use_cutoff,op=set,value=20*variable.combustion_on_use&!variable.font_double_on_use+40*variable.font_double_on_use+25*equipped.azsharas_font_of_power&!variable.font_double_on_use",
        "Items that are used outside of Combustion are not used after this time if they would put a trinket used with Combustion on a sharded cooldown." );
      break;
    case MAGE_FROST:
      precombat->add_action( this, "Summon Water Elemental" );
      break;
    default:
      break;
  }

  precombat->add_action( "snapshot_stats" );
  precombat->add_action( "use_item,name=azsharas_font_of_power" );
  precombat->add_talent( this, "Mirror Image" );
  precombat->add_action( "potion" );

  switch ( specialization() )
  {
    case MAGE_ARCANE:
      precombat->add_action( this, "Arcane Blast" );
      break;
    case MAGE_FIRE:
      precombat->add_action( this, "Pyroblast" );
      break;
    case MAGE_FROST:
      precombat->add_action( this, "Frostbolt" );
      break;
    default:
      break;
  }
}

std::string mage_t::default_potion() const
{
  std::string lvl120_potion =
    ( specialization() == MAGE_ARCANE ) ? "focused_resolve" :
                                          "unbridled_fury";

  std::string lvl110_potion =
    ( specialization() == MAGE_ARCANE ) ? "deadly_grace" :
                                          "prolonged_power";

  return ( true_level > 110 ) ? lvl120_potion :
         ( true_level > 100 ) ? lvl110_potion :
         ( true_level >  90 ) ? "draenic_intellect" :
         ( true_level >  85 ) ? "jade_serpent" :
         ( true_level >  80 ) ? "volcanic" :
                                "disabled";
}

std::string mage_t::default_flask() const
{
  return ( true_level > 110 ) ? "greater_flask_of_endless_fathoms" :
         ( true_level > 100 ) ? "whispered_pact" :
         ( true_level >  90 ) ? "greater_draenic_intellect_flask" :
         ( true_level >  85 ) ? "warm_sun" :
         ( true_level >  80 ) ? "draconic_mind" :
                                "disabled";
}

std::string mage_t::default_food() const
{
  std::string lvl100_food;
  std::string lvl120_food;

  switch ( specialization() )
  {
    case MAGE_ARCANE:
      lvl100_food = "sleeper_sushi";
      lvl120_food = "mechdowels_big_mech";
      break;
    case MAGE_FIRE:
      lvl100_food = "pickled_eel";
      lvl120_food = "baked_port_tato";
      break;
    case MAGE_FROST:
      lvl100_food = "salty_squid_roll";
      switch ( options.rotation )
      {
        case ROTATION_STANDARD:
        case ROTATION_NO_ICE_LANCE:
          lvl120_food = "abyssalfried_rissole";
          break;
        case ROTATION_FROZEN_ORB:
          lvl120_food = "mechdowels_big_mech";
          break;
        default:
          break;
      }
      break;
    default:
      break;
  }

  return ( true_level > 110 ) ? lvl120_food :
         ( true_level > 100 ) ? "fancy_darkmoon_feast" :
         ( true_level >  90 ) ? lvl100_food :
         ( true_level >  89 ) ? "mogu_fish_stew" :
         ( true_level >  80 ) ? "severed_sagefish_head" :
                                "disabled";
}

std::string mage_t::default_rune() const
{
  return ( true_level >= 120 ) ? "battle_scarred" :
         ( true_level >= 110 ) ? "defiled" :
         ( true_level >= 100 ) ? "focus" :
                                 "disabled";
}

void mage_t::apl_arcane()
{
  std::vector<std::string> racial_actions = get_racial_actions();

  action_priority_list_t* default_list = get_action_priority_list( "default" );
  action_priority_list_t* conserve     = get_action_priority_list( "conserve" );
  action_priority_list_t* burn         = get_action_priority_list( "burn" );
  action_priority_list_t* movement     = get_action_priority_list( "movement" );
  action_priority_list_t* essences     = get_action_priority_list( "essences" );


  default_list->add_action( this, "Counterspell" );
  default_list->add_action( "call_action_list,name=essences" );
  default_list->add_action( "call_action_list,name=burn,if=burn_phase|target.time_to_die<variable.average_burn_length", "Go to Burn Phase when already burning, or when boss will die soon." );
  default_list->add_action( "call_action_list,name=burn,if=(cooldown.arcane_power.remains=0&cooldown.evocation.remains<=variable.average_burn_length&(buff.arcane_charge.stack=buff.arcane_charge.max_stack|(talent.charged_up.enabled&cooldown.charged_up.remains=0&buff.arcane_charge.stack<=1)))", "Start Burn Phase when Arcane Power is ready and Evocation will be ready (on average) before the burn phase is over. Also make sure we got 4 Arcane Charges, or can get 4 Arcane Charges with Charged Up." );
  default_list->add_action( "call_action_list,name=conserve,if=!burn_phase" );
  default_list->add_action( "call_action_list,name=movement" );

  essences->add_action( "blood_of_the_enemy,if=burn_phase&buff.arcane_power.down&buff.rune_of_power.down&buff.arcane_charge.stack=buff.arcane_charge.max_stack|time_to_die<cooldown.arcane_power.remains" );
  essences->add_action( "concentrated_flame,line_cd=6,if=buff.rune_of_power.down&buff.arcane_power.down&(!burn_phase|time_to_die<cooldown.arcane_power.remains)&mana.time_to_max>=execute_time" );
  essences->add_action( "focused_azerite_beam,if=buff.rune_of_power.down&buff.arcane_power.down" );
  essences->add_action( "guardian_of_azeroth,if=buff.rune_of_power.down&buff.arcane_power.down" );
  essences->add_action( "purifying_blast,if=buff.rune_of_power.down&buff.arcane_power.down" );
  essences->add_action( "ripple_in_space,if=buff.rune_of_power.down&buff.arcane_power.down" );
  essences->add_action( "the_unbound_force,if=buff.rune_of_power.down&buff.arcane_power.down" );
  essences->add_action( "memory_of_lucid_dreams,if=!burn_phase&buff.arcane_power.down&cooldown.arcane_power.remains&buff.arcane_charge.stack=buff.arcane_charge.max_stack&(!talent.rune_of_power.enabled|action.rune_of_power.charges)|time_to_die<cooldown.arcane_power.remains" );
  essences->add_action( "worldvein_resonance,if=burn_phase&buff.arcane_power.down&buff.rune_of_power.down&buff.arcane_charge.stack=buff.arcane_charge.max_stack|time_to_die<cooldown.arcane_power.remains" );

  burn->add_action( "variable,name=total_burns,op=add,value=1,if=!burn_phase", "Increment our burn phase counter. Whenever we enter the `burn` actions without being in a burn phase, it means that we are about to start one." );
  burn->add_action( "start_burn_phase,if=!burn_phase" );
  burn->add_action( "stop_burn_phase,if=burn_phase&prev_gcd.1.evocation&target.time_to_die>variable.average_burn_length&burn_phase_duration>0", "End the burn phase when we just evocated." );
  burn->add_talent( this, "Charged Up", "if=buff.arcane_charge.stack<=1", "Less than 1 instead of equals to 0, because of pre-cast Arcane Blast" );
  burn->add_talent( this, "Mirror Image" );
  burn->add_talent( this, "Nether Tempest", "if=(refreshable|!ticking)&buff.arcane_charge.stack=buff.arcane_charge.max_stack&buff.rune_of_power.down&buff.arcane_power.down" );
  burn->add_action( this, "Arcane Blast", "if=buff.rule_of_threes.up&talent.overpowered.enabled&active_enemies<3",
    "When running Overpowered, and we got a Rule of Threes proc (AKA we got our 4th Arcane Charge via "
    "Charged Up), use it before using RoP+AP, because the mana reduction is otherwise largely wasted "
    "since the AB was free anyway." );
  burn->add_action( "lights_judgment,if=buff.arcane_power.down" );
  burn->add_action( "use_item,name=azsharas_font_of_power,if=cooldown.arcane_power.remains<5|time_to_die<cooldown.arcane_power.remains" );
  burn->add_talent( this, "Rune of Power", "if=!buff.arcane_power.up&(mana.pct>=50|cooldown.arcane_power.remains=0)&(buff.arcane_charge.stack=buff.arcane_charge.max_stack)" );
  burn->add_action( "berserking" );
  burn->add_action( this, "Arcane Power" );
  burn->add_action( "use_items,if=buff.arcane_power.up|target.time_to_die<cooldown.arcane_power.remains" );
  for ( const auto& ra : racial_actions )
  {
    if ( ra == "lights_judgment" || ra == "arcane_torrent" || ra == "berserking" )
      continue;  // Handled manually.

    burn->add_action( ra );
  }
  burn->add_action( this, "Presence of Mind", "if=(talent.rune_of_power.enabled&buff.rune_of_power.remains<=buff.presence_of_mind.max_stack*action.arcane_blast.execute_time)|buff.arcane_power.remains<=buff.presence_of_mind.max_stack*action.arcane_blast.execute_time" );
  burn->add_action( "potion,if=buff.arcane_power.up&(buff.berserking.up|buff.blood_fury.up|!(race.troll|race.orc))" );
  burn->add_talent( this, "Arcane Orb", "if=buff.arcane_charge.stack=0|(active_enemies<3|(active_enemies<2&talent.resonance.enabled))" );
  burn->add_action( this, "Arcane Barrage", "if=active_enemies>=3&(buff.arcane_charge.stack=buff.arcane_charge.max_stack)" );
  burn->add_action( this, "Arcane Explosion", "if=active_enemies>=3" );
  burn->add_action( this, "Arcane Missiles", "if=buff.clearcasting.react&active_enemies<3&(talent.amplification.enabled|(!talent.overpowered.enabled&azerite.arcane_pummeling.rank>=2)|buff.arcane_power.down),chain=1", "Ignore Arcane Missiles during Arcane Power, aside from some very specific exceptions, like not having Overpowered talented & running 3x Arcane Pummeling." );
  burn->add_action( this, "Arcane Blast", "if=active_enemies<3" );
  burn->add_action( "variable,name=average_burn_length,op=set,value=(variable.average_burn_length*variable.total_burns-variable.average_burn_length+(burn_phase_duration))%variable.total_burns", "Now that we're done burning, we can update the average_burn_length with the length of this burn." );
  burn->add_action( this, "Evocation", "interrupt_if=mana.pct>=85,interrupt_immediate=1" );
  burn->add_action( this, "Arcane Barrage", "", "For the rare occasion where we go oom before evocation is back up. (Usually because we get very bad rng so the burn is cut very short)" );

  conserve->add_talent( this, "Mirror Image" );
  conserve->add_talent( this, "Charged Up", "if=buff.arcane_charge.stack=0" );
  conserve->add_talent( this, "Nether Tempest", "if=(refreshable|!ticking)&buff.arcane_charge.stack=buff.arcane_charge.max_stack&buff.rune_of_power.down&buff.arcane_power.down" );
  conserve->add_talent( this, "Arcane Orb", "if=buff.arcane_charge.stack<=2&(cooldown.arcane_power.remains>10|active_enemies<=2)" );
  conserve->add_action( this, "Arcane Blast", "if=buff.rule_of_threes.up&buff.arcane_charge.stack>3", "Arcane Blast shifts up in priority when running rule of threes." );
  conserve->add_action( "use_item,name=tidestorm_codex,if=buff.rune_of_power.down&!buff.arcane_power.react&cooldown.arcane_power.remains>20" );
  conserve->add_action( "use_item,effect_name=cyclotronic_blast,if=buff.rune_of_power.down&!buff.arcane_power.react&cooldown.arcane_power.remains>20" );
  conserve->add_talent( this, "Rune of Power", "if=buff.arcane_charge.stack=buff.arcane_charge.max_stack&(full_recharge_time<=execute_time|full_recharge_time<=cooldown.arcane_power.remains|target.time_to_die<=cooldown.arcane_power.remains)" );
  conserve->add_action( this, "Arcane Missiles", "if=mana.pct<=95&buff.clearcasting.react&active_enemies<3,chain=1" );
  conserve->add_action( this, "Arcane Barrage", "if=((buff.arcane_charge.stack=buff.arcane_charge.max_stack)&((mana.pct<=variable.conserve_mana)|(talent.rune_of_power.enabled&cooldown.arcane_power.remains>cooldown.rune_of_power.full_recharge_time&mana.pct<=variable.conserve_mana+25))|(talent.arcane_orb.enabled&cooldown.arcane_orb.remains<=gcd&cooldown.arcane_power.remains>10))|mana.pct<=(variable.conserve_mana-10)", "During conserve, we still just want to continue not dropping charges as long as possible.So keep 'burning' as long as possible (aka conserve_mana threshhold) and then swap to a 4x AB->Abarr conserve rotation. If we do not have 4 AC, we can dip slightly lower to get a 4th AC. We also sustain at a higher mana percentage when we plan to use a Rune of Power during conserve phase, so we can burn during the Rune of Power." );
  conserve->add_talent( this, "Supernova", "if=mana.pct<=95", "Supernova is barely worth casting, which is why it is so far down, only just above AB. " );
  conserve->add_action( this, "Arcane Explosion", "if=active_enemies>=3&(mana.pct>=variable.conserve_mana|buff.arcane_charge.stack=3)", "Keep 'burning' in aoe situations until conserve_mana pct. After that only cast AE with 3 Arcane charges, since it's almost equal mana cost to a 3 stack AB anyway. At that point AoE rotation will be AB x3->AE->Abarr" );
  conserve->add_action( this, "Arcane Blast" );
  conserve->add_action( this, "Arcane Barrage" );

  movement->add_action( "blink_any,if=movement.distance>=10" );
  movement->add_action( this, "Presence of Mind" );
  movement->add_action( this, "Arcane Missiles" );
  movement->add_talent( this, "Arcane Orb" );
  movement->add_talent( this, "Supernova" );
}

void mage_t::apl_fire()
{
  std::vector<std::string> racial_actions = get_racial_actions();

  action_priority_list_t* default_list        = get_action_priority_list( "default"             );
  action_priority_list_t* combustion_phase    = get_action_priority_list( "combustion_phase"    );
  action_priority_list_t* rop_phase           = get_action_priority_list( "rop_phase"           );
  action_priority_list_t* active_talents      = get_action_priority_list( "active_talents"      );
  action_priority_list_t* items_low_priority  = get_action_priority_list( "items_low_priority"  );
  action_priority_list_t* items_high_priority = get_action_priority_list( "items_high_priority" );
  action_priority_list_t* items_combustion    = get_action_priority_list( "items_combustion"    );
  action_priority_list_t* standard            = get_action_priority_list( "standard_rotation"   );

  default_list->add_action( this, "Counterspell" );
  default_list->add_action( "call_action_list,name=items_high_priority" );
  default_list->add_talent( this, "Mirror Image", "if=buff.combustion.down" );
  default_list->add_action( "guardian_of_azeroth,if=cooldown.combustion.remains<10|target.time_to_die<cooldown.combustion.remains" );
  default_list->add_action( "concentrated_flame" );
  default_list->add_action( "focused_azerite_beam" );
  default_list->add_action( "purifying_blast" );
  default_list->add_action( "ripple_in_space" );
  default_list->add_action( "the_unbound_force" );
  default_list->add_action( "worldvein_resonance" );
  default_list->add_talent( this, "Rune of Power", "if=talent.firestarter.enabled&firestarter.remains>full_recharge_time|cooldown.combustion.remains>variable.combustion_rop_cutoff&buff.combustion.down|target.time_to_die<cooldown.combustion.remains&buff.combustion.down" );
  default_list->add_action( "call_action_list,name=combustion_phase,if=(talent.rune_of_power.enabled&cooldown.combustion.remains<=action.rune_of_power.cast_time|cooldown.combustion.ready)&!firestarter.active|buff.combustion.up" );
  default_list->add_action( this, "Fire Blast", "use_while_casting=1,use_off_gcd=1,if=(essence.memory_of_lucid_dreams.major|essence.memory_of_lucid_dreams.minor&azerite.blaster_master.enabled)&charges=max_charges&!buff.hot_streak.react&!(buff.heating_up.react&(buff.combustion.up&(action.fireball.in_flight|action.pyroblast.in_flight|action.scorch.executing)|target.health.pct<=30&action.scorch.executing))&!(!buff.heating_up.react&!buff.hot_streak.react&buff.combustion.down&(action.fireball.in_flight|action.pyroblast.in_flight))" );
  default_list->add_action( this, "Fire Blast", "use_while_casting=1,use_off_gcd=1,if=firestarter.active&charges>=1&(!variable.fire_blast_pooling|buff.rune_of_power.up)&(!azerite.blaster_master.enabled|buff.blaster_master.remains<0.5)&(!action.fireball.executing&!action.pyroblast.in_flight&buff.heating_up.up|action.fireball.executing&buff.hot_streak.down|action.pyroblast.in_flight&buff.heating_up.down&buff.hot_streak.down)",
    "During Firestarter, Fire Blasts are used similarly to during Combustion. Generally, they are used to generate Hot Streaks when crits will not be wasted and with Blaster Master, they should be spread out to maintain the Blaster Master buff." );
  default_list->add_action( "call_action_list,name=rop_phase,if=buff.rune_of_power.up&buff.combustion.down" );
  default_list->add_action( "variable,name=fire_blast_pooling,value=talent.rune_of_power.enabled&cooldown.rune_of_power.remains<cooldown.fire_blast.full_recharge_time&(cooldown.combustion.remains>variable.combustion_rop_cutoff|firestarter.active)&(cooldown.rune_of_power.remains<target.time_to_die|action.rune_of_power.charges>0)|cooldown.combustion.remains<action.fire_blast.full_recharge_time+cooldown.fire_blast.duration*azerite.blaster_master.enabled&!firestarter.active&cooldown.combustion.remains<target.time_to_die|talent.firestarter.enabled&firestarter.active&firestarter.remains<cooldown.fire_blast.full_recharge_time+cooldown.fire_blast.duration*azerite.blaster_master.enabled" );
  default_list->add_action( "variable,name=phoenix_pooling,value=talent.rune_of_power.enabled&cooldown.rune_of_power.remains<cooldown.phoenix_flames.full_recharge_time&cooldown.combustion.remains>variable.combustion_rop_cutoff&(cooldown.rune_of_power.remains<target.time_to_die|action.rune_of_power.charges>0)|cooldown.combustion.remains<action.phoenix_flames.full_recharge_time&cooldown.combustion.remains<target.time_to_die" );
  default_list->add_action( "call_action_list,name=standard_rotation" );

  active_talents->add_talent( this, "Living Bomb", "if=active_enemies>1&buff.combustion.down&(cooldown.combustion.remains>cooldown.living_bomb.duration|cooldown.combustion.ready)" );
  active_talents->add_talent( this, "Meteor", "if=buff.rune_of_power.up&(firestarter.remains>cooldown.meteor.duration|!firestarter.active)|cooldown.rune_of_power.remains>target.time_to_die&action.rune_of_power.charges<1|(cooldown.meteor.duration<cooldown.combustion.remains|cooldown.combustion.ready)&!talent.rune_of_power.enabled&(cooldown.meteor.duration<firestarter.remains|!talent.firestarter.enabled|!firestarter.active)" );
  active_talents->add_talent( this, "Dragon's Breath", "if=talent.alexstraszas_fury.enabled&(buff.combustion.down&!buff.hot_streak.react|buff.combustion.up&action.fire_blast.charges<action.fire_blast.max_charges&!buff.hot_streak.react)" );

  combustion_phase->add_action( "lights_judgment,if=buff.combustion.down", "Combustion phase prepares abilities with a delay, then launches into the Combustion sequence" );
  combustion_phase->add_action( "blood_of_the_enemy" );
  combustion_phase->add_action( "memory_of_lucid_dreams" );
  combustion_phase->add_action( this, "Fire Blast", "use_while_casting=1,use_off_gcd=1,if=charges>=1&((action.fire_blast.charges_fractional+(buff.combustion.remains-buff.blaster_master.duration)%cooldown.fire_blast.duration-(buff.combustion.remains)%(buff.blaster_master.duration-0.5))>=0|!azerite.blaster_master.enabled|!talent.flame_on.enabled|buff.combustion.remains<=buff.blaster_master.duration|buff.blaster_master.remains<0.5|equipped.hyperthread_wristwraps&cooldown.hyperthread_wristwraps_300142.remains<5)&buff.combustion.up&(!action.scorch.executing&!action.pyroblast.in_flight&buff.heating_up.up|action.scorch.executing&buff.hot_streak.down&(buff.heating_up.down|azerite.blaster_master.enabled)|azerite.blaster_master.enabled&talent.flame_on.enabled&action.pyroblast.in_flight&buff.heating_up.down&buff.hot_streak.down)",
    "During Combustion, Fire Blasts are used to generate Hot Streaks and minimize the amount of time spent executing other spells. "
    "For standard Fire, Fire Blasts are only used when Heating Up is active or when a Scorch cast is in progress and Heating Up and Hot Streak are not active. "
    "With Blaster Master and Flame On, Fire Blasts can additionally be used while Hot Streak and Heating Up are not active and a Pyroblast is in the air "
    "and also while casting Scorch even if Heating Up is already active. The latter allows two Hot Streak Pyroblasts to be cast in succession after the Scorch. "
    "Additionally with Blaster Master and Flame On, Fire Blasts should not be used unless Blaster Master is about to expire "
    "or there are more than enough Fire Blasts to extend Blaster Master to the end of Combustion." ); 
  combustion_phase->add_talent( this, "Rune of Power", "if=buff.combustion.down" );
  combustion_phase->add_action( this, "Fire Blast", "use_while_casting=1,if=azerite.blaster_master.enabled&talent.flame_on.enabled&buff.blaster_master.down&(talent.rune_of_power.enabled&action.rune_of_power.executing&action.rune_of_power.execute_remains<0.6|(cooldown.combustion.ready|buff.combustion.up)&!talent.rune_of_power.enabled&!action.pyroblast.in_flight&!action.fireball.in_flight)",
    "With Blaster Master, a Fire Blast should be used while casting Rune of Power." );
  combustion_phase->add_action( "call_action_list,name=active_talents" );
  combustion_phase->add_action( this, "Combustion", "use_off_gcd=1,use_while_casting=1,if=((action.meteor.in_flight&action.meteor.in_flight_remains<=0.5)|!talent.meteor.enabled)&(buff.rune_of_power.up|!talent.rune_of_power.enabled)" );
  combustion_phase->add_action( "potion" );
  for ( const auto& ra : racial_actions )
  {
    if ( ra == "lights_judgment" || ra == "arcane_torrent" )
      continue;  // Handled manually.

    combustion_phase->add_action( ra );
  }
  combustion_phase->add_action( this, "Flamestrike", "if=((talent.flame_patch.enabled&active_enemies>2)|active_enemies>6)&buff.hot_streak.react&!azerite.blaster_master.enabled" );
  combustion_phase->add_action( this, "Pyroblast", "if=buff.pyroclasm.react&buff.combustion.remains>cast_time" );
  combustion_phase->add_action( this, "Pyroblast", "if=buff.hot_streak.react" );
  combustion_phase->add_action( this, "Pyroblast", "if=prev_gcd.1.scorch&buff.heating_up.up" );
  combustion_phase->add_talent( this, "Phoenix Flames" );
  combustion_phase->add_action( this, "Scorch", "if=buff.combustion.remains>cast_time&buff.combustion.up|buff.combustion.down" );
  combustion_phase->add_talent( this, "Living Bomb", "if=buff.combustion.remains<gcd.max&active_enemies>1" );
  combustion_phase->add_action( this, "Dragon's Breath", "if=buff.combustion.remains<gcd.max&buff.combustion.up" );
  combustion_phase->add_action( this, "Scorch", "if=target.health.pct<=30&talent.searing_touch.enabled" );

  rop_phase->add_talent( this, "Rune of Power" );
  rop_phase->add_action( this, "Flamestrike", "if=(talent.flame_patch.enabled&active_enemies>1|active_enemies>4)&buff.hot_streak.react" );
  rop_phase->add_action( this, "Pyroblast", "if=buff.hot_streak.react" );
  rop_phase->add_action( this, "Fire Blast", "use_off_gcd=1,use_while_casting=1,if=!(talent.flame_patch.enabled&active_enemies>2|active_enemies>5)&(!firestarter.active&cooldown.combustion.remains>0)&(!buff.heating_up.react&!buff.hot_streak.react&!prev_off_gcd.fire_blast&(action.fire_blast.charges>=2|(action.phoenix_flames.charges>=1&talent.phoenix_flames.enabled)|(talent.alexstraszas_fury.enabled&cooldown.dragons_breath.ready)|(talent.searing_touch.enabled&target.health.pct<=30)))" );
  rop_phase->add_action( "call_action_list,name=active_talents" );
  rop_phase->add_action( this, "Pyroblast", "if=buff.pyroclasm.react&cast_time<buff.pyroclasm.remains&buff.rune_of_power.remains>cast_time" );
  rop_phase->add_action( this, "Fire Blast", "use_off_gcd=1,use_while_casting=1,if=!(talent.flame_patch.enabled&active_enemies>2|active_enemies>5)&(!firestarter.active&cooldown.combustion.remains>0)&(buff.heating_up.react&(target.health.pct>=30|!talent.searing_touch.enabled))" );
  rop_phase->add_action( this, "Fire Blast", "use_off_gcd=1,use_while_casting=1,if=!(talent.flame_patch.enabled&active_enemies>2|active_enemies>5)&(!firestarter.active&cooldown.combustion.remains>0)&talent.searing_touch.enabled&target.health.pct<=30&(buff.heating_up.react&!action.scorch.executing|!buff.heating_up.react&!buff.hot_streak.react)" );
  rop_phase->add_action( this, "Pyroblast", "if=prev_gcd.1.scorch&buff.heating_up.up&talent.searing_touch.enabled&target.health.pct<=30&(!talent.flame_patch.enabled|active_enemies=1)" );
  rop_phase->add_talent( this, "Phoenix Flames", "if=!prev_gcd.1.phoenix_flames&buff.heating_up.react" );
  rop_phase->add_action( this, "Scorch", "if=target.health.pct<=30&talent.searing_touch.enabled" );
  rop_phase->add_action( this, "Dragon's Breath", "if=active_enemies>2" );
  rop_phase->add_action( this, "Fire Blast", "use_off_gcd=1,use_while_casting=1,if=(talent.flame_patch.enabled&active_enemies>2|active_enemies>5)&(cooldown.combustion.remains>0&!firestarter.active)&buff.hot_streak.down&(!azerite.blaster_master.enabled|buff.blaster_master.remains<0.5)",
    "When Hardcasting Flame Strike, Fire Blasts should be used to generate Hot Streaks and to extend Blaster Master." );
  rop_phase->add_action( this, "Flamestrike", "if=talent.flame_patch.enabled&active_enemies>2|active_enemies>5" );
  rop_phase->add_action( this, "Fireball" );

  standard->add_action( this, "Flamestrike", "if=((talent.flame_patch.enabled&active_enemies>1&!firestarter.active)|active_enemies>4)&buff.hot_streak.react" );
  standard->add_action( this, "Pyroblast", "if=buff.hot_streak.react&buff.hot_streak.remains<action.fireball.execute_time" );
  standard->add_action( this, "Pyroblast", "if=buff.hot_streak.react&(prev_gcd.1.fireball|firestarter.active|action.pyroblast.in_flight)" );
  standard->add_talent( this, "Phoenix Flames", "if=charges>=3&active_enemies>2&!variable.phoenix_pooling" );
  standard->add_action( this, "Pyroblast", "if=buff.hot_streak.react&target.health.pct<=30&talent.searing_touch.enabled" );
  standard->add_action( this, "Pyroblast", "if=buff.pyroclasm.react&cast_time<buff.pyroclasm.remains" );
  standard->add_action( this, "Fire Blast", "use_off_gcd=1,use_while_casting=1,if=(cooldown.combustion.remains>0&buff.rune_of_power.down&!firestarter.active)&!talent.kindling.enabled&!variable.fire_blast_pooling&(((action.fireball.executing|action.pyroblast.executing)&(buff.heating_up.react))|(talent.searing_touch.enabled&target.health.pct<=30&(buff.heating_up.react&!action.scorch.executing|!buff.hot_streak.react&!buff.heating_up.react&action.scorch.executing&!action.pyroblast.in_flight&!action.fireball.in_flight)))" );
  standard->add_action( this, "Fire Blast", "if=talent.kindling.enabled&buff.heating_up.react&!firestarter.active&(cooldown.combustion.remains>full_recharge_time+2+talent.kindling.enabled|(!talent.rune_of_power.enabled|cooldown.rune_of_power.remains>target.time_to_die&action.rune_of_power.charges<1)&cooldown.combustion.remains>target.time_to_die)" );
  standard->add_action( this, "Pyroblast", "if=prev_gcd.1.scorch&buff.heating_up.up&talent.searing_touch.enabled&target.health.pct<=30&((talent.flame_patch.enabled&active_enemies=1&!firestarter.active)|(active_enemies<4&!talent.flame_patch.enabled))" );
  standard->add_talent( this, "Phoenix Flames", "if=(buff.heating_up.react|(!buff.hot_streak.react&(action.fire_blast.charges>0|talent.searing_touch.enabled&target.health.pct<=30)))&!variable.phoenix_pooling" );
  standard->add_action( "call_action_list,name=active_talents" );
  standard->add_action( this, "Dragon's Breath", "if=active_enemies>1" );
  standard->add_action( "call_action_list,name=items_low_priority" );
  standard->add_action( this, "Scorch", "if=target.health.pct<=30&talent.searing_touch.enabled" );
  standard->add_action( this, "Fire Blast", "use_off_gcd=1,use_while_casting=1,if=(talent.flame_patch.enabled&active_enemies>2|active_enemies>9)&(cooldown.combustion.remains>0&!firestarter.active)&buff.hot_streak.down&(!azerite.blaster_master.enabled|buff.blaster_master.remains<0.5)",
    "When Hardcasting Flame Strike, Fire Blasts should be used to generate Hot Streaks and to extend Blaster Master." );
  standard->add_action( this, "Flamestrike", "if=talent.flame_patch.enabled&active_enemies>2|active_enemies>9",
    "With enough targets, it is a gain to cast Flamestrike as filler instead of Fireball." );
  standard->add_action( this, "Fireball" );
  standard->add_action( this, "Scorch" );

  items_low_priority->add_action( "use_item,name=tidestorm_codex,if=cooldown.combustion.remains>variable.on_use_cutoff|talent.firestarter.enabled&firestarter.remains>variable.on_use_cutoff" );
  items_low_priority->add_action( "use_item,effect_name=cyclotronic_blast,if=cooldown.combustion.remains>variable.on_use_cutoff|talent.firestarter.enabled&firestarter.remains>variable.on_use_cutoff" );

  items_high_priority->add_action( "call_action_list,name=items_combustion,if=(talent.rune_of_power.enabled&cooldown.combustion.remains<=action.rune_of_power.cast_time|cooldown.combustion.ready)&!firestarter.active|buff.combustion.up" );
  items_high_priority->add_action( "use_items" );
  items_high_priority->add_action( "use_item,name=azsharas_font_of_power,if=cooldown.combustion.remains<=5+15*variable.font_double_on_use" );
  items_high_priority->add_action( "use_item,name=rotcrusted_voodoo_doll,if=cooldown.combustion.remains>variable.on_use_cutoff" );
  items_high_priority->add_action( "use_item,name=aquipotent_nautilus,if=cooldown.combustion.remains>variable.on_use_cutoff" );
  items_high_priority->add_action( "use_item,name=shiver_venom_relic,if=cooldown.combustion.remains>variable.on_use_cutoff" );
  items_high_priority->add_action( "use_item,effect_name=harmonic_dematerializer" );
  items_high_priority->add_action( "use_item,name=malformed_heralds_legwraps,if=cooldown.combustion.remains>=55&buff.combustion.down&cooldown.combustion.remains>variable.on_use_cutoff" );
  items_high_priority->add_action( "use_item,name=ancient_knot_of_wisdom,if=cooldown.combustion.remains>=55&buff.combustion.down&cooldown.combustion.remains>variable.on_use_cutoff" );
  items_high_priority->add_action( "use_item,name=neural_synapse_enhancer,if=cooldown.combustion.remains>=45&buff.combustion.down&cooldown.combustion.remains>variable.on_use_cutoff" );

  items_combustion->add_action( "use_item,name=ignition_mages_fuse" );
  items_combustion->add_action( "use_item,name=hyperthread_wristwraps,if=buff.combustion.up&action.fire_blast.charges=0&action.fire_blast.recharge_time>gcd.max" );
  items_combustion->add_action( "use_item,use_off_gcd=1,name=azurethos_singed_plumage,if=buff.combustion.up|action.meteor.in_flight&action.meteor.in_flight_remains<=0.5" );
  items_combustion->add_action( "use_item,use_off_gcd=1,effect_name=gladiators_badge,if=buff.combustion.up|action.meteor.in_flight&action.meteor.in_flight_remains<=0.5" );
  items_combustion->add_action( "use_item,use_off_gcd=1,effect_name=gladiators_medallion,if=buff.combustion.up|action.meteor.in_flight&action.meteor.in_flight_remains<=0.5" );
  items_combustion->add_action( "use_item,use_off_gcd=1,name=balefire_branch,if=buff.combustion.up|action.meteor.in_flight&action.meteor.in_flight_remains<=0.5" );
  items_combustion->add_action( "use_item,use_off_gcd=1,name=shockbiters_fang,if=buff.combustion.up|action.meteor.in_flight&action.meteor.in_flight_remains<=0.5" );
  items_combustion->add_action( "use_item,use_off_gcd=1,name=tzanes_barkspines,if=buff.combustion.up|action.meteor.in_flight&action.meteor.in_flight_remains<=0.5" );
  items_combustion->add_action( "use_item,use_off_gcd=1,name=ancient_knot_of_wisdom,if=buff.combustion.up|action.meteor.in_flight&action.meteor.in_flight_remains<=0.5" );
  items_combustion->add_action( "use_item,use_off_gcd=1,name=neural_synapse_enhancer,if=buff.combustion.up|action.meteor.in_flight&action.meteor.in_flight_remains<=0.5" );
  items_combustion->add_action( "use_item,use_off_gcd=1,name=malformed_heralds_legwraps,if=buff.combustion.up|action.meteor.in_flight&action.meteor.in_flight_remains<=0.5" );
}

void mage_t::apl_frost()
{
  std::vector<std::string> racial_actions = get_racial_actions();

  action_priority_list_t* default_list = get_action_priority_list( "default"    );
  action_priority_list_t* single       = get_action_priority_list( "single"     );
  action_priority_list_t* aoe          = get_action_priority_list( "aoe"        );
  action_priority_list_t* cooldowns    = get_action_priority_list( "cooldowns"  );
  action_priority_list_t* essences     = get_action_priority_list( "essences"   );
  action_priority_list_t* movement     = get_action_priority_list( "movement"   );
  action_priority_list_t* talent_rop   = get_action_priority_list( "talent_rop" );

  default_list->add_action( this, "Counterspell" );
  if ( options.rotation != ROTATION_NO_ICE_LANCE )
  {
    default_list->add_action( this, "Ice Lance", "if=prev_gcd.1.flurry&!buff.fingers_of_frost.react",
      "If the mage has FoF after casting instant Flurry, we can delay the Ice Lance and use other high priority action, if available." );
  }
  default_list->add_action( "call_action_list,name=cooldowns" );
  default_list->add_action( "call_action_list,name=aoe,if=active_enemies>3&talent.freezing_rain.enabled|active_enemies>4",
    "The target threshold isn't exact. Between 3-5 targets, the differences between the ST and AoE action lists are rather small. "
    "However, Freezing Rain prefers using AoE action list sooner as it benefits greatly from the high priority Blizzard action." );
  default_list->add_action( "call_action_list,name=single" );

  single->add_talent( this, "Ice Nova", "if=cooldown.ice_nova.ready&debuff.winters_chill.up",
    "In some situations, you can shatter Ice Nova even after already casting Flurry and Ice Lance. "
    "Otherwise this action is used when the mage has FoF after casting Flurry, see above." );
  switch ( options.rotation )
  {
    case ROTATION_STANDARD:
      single->add_action( this, "Flurry", "if=talent.ebonbolt.enabled&prev_gcd.1.ebonbolt&(!talent.glacial_spike.enabled|buff.icicles.stack<4|buff.brain_freeze.react)",
        "Without GS, Ebonbolt is always shattered. With GS, Ebonbolt is shattered if it would waste Brain Freeze charge (i.e. when the "
        "mage starts casting Ebonbolt with Brain Freeze active) or when below 4 Icicles (if Ebonbolt is cast when the mage has 4-5 Icicles, "
        "it's better to use the Brain Freeze from it on Glacial Spike)." );
      single->add_action( this, "Flurry", "if=talent.glacial_spike.enabled&prev_gcd.1.glacial_spike&buff.brain_freeze.react",
        "Glacial Spike is always shattered." );
      single->add_action( this, "Flurry", "if=prev_gcd.1.frostbolt&buff.brain_freeze.react&(!talent.glacial_spike.enabled|buff.icicles.stack<4)",
        "Without GS, the mage just tries to shatter as many Frostbolts as possible. With GS, the mage only shatters Frostbolt that would "
        "put them at 1-3 Icicle stacks. Difference between shattering Frostbolt with 1-3 Icicles and 1-4 Icicles is small, but 1-3 tends "
        "to be better in more situations (the higher GS damage is, the more it leans towards 1-3). Forcing shatter on Frostbolt is still "
        "a small gain, so is not caring about FoF. Ice Lance is too weak to warrant delaying Brain Freeze Flurry." );
      single->add_action( "call_action_list,name=essences" );
      single->add_action( this, "Frozen Orb" );
      single->add_action( this, "Blizzard", "if=active_enemies>2|active_enemies>1&cast_time=0&buff.fingers_of_frost.react<2",
        "With Freezing Rain and at least 2 targets, Blizzard needs to be used with higher priority to make sure you can fit both instant Blizzards "
        "into a single Freezing Rain. Starting with three targets, Blizzard leaves the low priority filler role and is used on cooldown (and just making "
        "sure not to waste Brain Freeze charges) with or without Freezing Rain." );
      single->add_action( this, "Ice Lance", "if=buff.fingers_of_frost.react",
        "Trying to pool charges of FoF for anything isn't worth it. Use them as they come." );
      single->add_talent( this, "Comet Storm" );
      single->add_talent( this, "Ebonbolt" );
      single->add_talent( this, "Ray of Frost", "if=!action.frozen_orb.in_flight&ground_aoe.frozen_orb.remains=0",
        "Ray of Frost is used after all Fingers of Frost charges have been used and there isn't active Frozen Orb that could generate more. "
        "This is only a small gain against multiple targets, as Ray of Frost isn't too impactful." );
      single->add_action( this, "Blizzard", "if=cast_time=0|active_enemies>1",
        "Blizzard is used as low priority filler against 2 targets. When using Freezing Rain, it's a medium gain to use the instant Blizzard even "
        "against a single target, especially with low mastery." );
      single->add_talent( this, "Glacial Spike", "if=buff.brain_freeze.react|prev_gcd.1.ebonbolt|active_enemies>1&talent.splitting_ice.enabled",
        "Glacial Spike is used when there's a Brain Freeze proc active (i.e. only when it can be shattered). This is a small to medium gain "
        "in most situations. Low mastery leans towards using it when available. When using Splitting Ice and having another target nearby, "
        "it's slightly better to use GS when available, as the second target doesn't benefit from shattering the main target." );
      break;
    case ROTATION_NO_ICE_LANCE:
      single->add_action( this, "Flurry", "if=talent.ebonbolt.enabled&prev_gcd.1.ebonbolt&buff.brain_freeze.react" );
      single->add_action( this, "Flurry", "if=prev_gcd.1.glacial_spike&buff.brain_freeze.react" );
      single->add_action( "call_action_list,name=essences" );
      single->add_action( this, "Frozen Orb" );
      single->add_action( this, "Blizzard", "if=active_enemies>2|active_enemies>1&!talent.splitting_ice.enabled" );
      single->add_talent( this, "Comet Storm" );
      single->add_talent( this, "Ebonbolt", "if=buff.icicles.stack=5&!buff.brain_freeze.react" );
      single->add_talent( this, "Glacial Spike", "if=buff.brain_freeze.react|prev_gcd.1.ebonbolt"
        "|talent.incanters_flow.enabled&cast_time+travel_time>incanters_flow_time_to.5.up&cast_time+travel_time<incanters_flow_time_to.4.down" );
      break;
    case ROTATION_FROZEN_ORB:
      single->add_action( "call_action_list,name=essences" );
      single->add_action( this, "Frozen Orb" );
      single->add_action( this, "Flurry", "if=prev_gcd.1.ebonbolt&buff.brain_freeze.react" );
      single->add_action( this, "Blizzard", "if=active_enemies>2|active_enemies>1&cast_time=0" );
      single->add_action( this, "Ice Lance", "if=buff.fingers_of_frost.react&cooldown.frozen_orb.remains>5|buff.fingers_of_frost.react=2" );
      single->add_action( this, "Blizzard", "if=cast_time=0" );
      single->add_action( this, "Flurry", "if=prev_gcd.1.ebonbolt" );
      single->add_action( this, "Flurry", "if=buff.brain_freeze.react&(prev_gcd.1.frostbolt|debuff.packed_ice.remains>execute_time+"
        "action.ice_lance.travel_time)" );
      single->add_talent( this, "Comet Storm" );
      single->add_talent( this, "Ebonbolt" );
      single->add_talent( this, "Ray of Frost", "if=debuff.packed_ice.up,interrupt_if=buff.fingers_of_frost.react=2,interrupt_immediate=1" );
      single->add_action( this, "Blizzard" );
      break;
    default:
      break;
  }
  single->add_talent( this, "Ice Nova" );
  single->add_action( "use_item,name=tidestorm_codex,if=buff.icy_veins.down&buff.rune_of_power.down" );
  single->add_action( "use_item,effect_name=cyclotronic_blast,if=buff.icy_veins.down&buff.rune_of_power.down" );
  single->add_action( this, "Frostbolt" );
  single->add_action( "call_action_list,name=movement" );
  single->add_action( this, "Ice Lance" );

  aoe->add_action( this, "Frozen Orb", "",
    "With Freezing Rain, it's better to prioritize using Frozen Orb when both FO and Blizzard are off cooldown. "
    "Without Freezing Rain, the converse is true although the difference is miniscule until very high target counts." );
  aoe->add_action( this, "Blizzard" );
  aoe->add_action( "call_action_list,name=essences" );
  aoe->add_talent( this, "Comet Storm" );
  aoe->add_talent( this, "Ice Nova" );
  aoe->add_action( this, "Flurry", "if=prev_gcd.1.ebonbolt|buff.brain_freeze.react&(prev_gcd.1.frostbolt&(buff.icicles.stack<4|!talent.glacial_spike.enabled)|prev_gcd.1.glacial_spike)",
    "Simplified Flurry conditions from the ST action list. Since the mage is generating far less Brain Freeze charges, the exact "
    "condition here isn't all that important." );
  aoe->add_action( this, "Ice Lance", "if=buff.fingers_of_frost.react" );
  aoe->add_talent( this, "Ray of Frost", "",
    "The mage will generally be generating a lot of FoF charges when using the AoE action list. Trying to delay Ray of Frost "
    "until there are no FoF charges and no active Frozen Orbs would lead to it not being used at all." );
  aoe->add_talent( this, "Ebonbolt" );
  aoe->add_talent( this, "Glacial Spike" );
  aoe->add_action( this, "Cone of Cold", "",
    "Using Cone of Cold is mostly DPS neutral with the AoE target thresholds. It only becomes decent gain with roughly 7 or more targets." );
  aoe->add_action( "use_item,name=tidestorm_codex,if=buff.icy_veins.down&buff.rune_of_power.down" );
  aoe->add_action( "use_item,effect_name=cyclotronic_blast,if=buff.icy_veins.down&buff.rune_of_power.down" );
  aoe->add_action( this, "Frostbolt" );
  aoe->add_action( "call_action_list,name=movement" );
  aoe->add_action( this, "Ice Lance" );

  cooldowns->add_action( options.rotation == ROTATION_FROZEN_ORB ? "guardian_of_azeroth,if=cooldown.frozen_orb.remains<5" : "guardian_of_azeroth" );
  cooldowns->add_action( this, "Icy Veins", options.rotation == ROTATION_FROZEN_ORB ? "if=cooldown.frozen_orb.remains<5" : "" );
  cooldowns->add_talent( this, "Mirror Image" );
  cooldowns->add_talent( this, "Rune of Power", "if=prev_gcd.1.frozen_orb|target.time_to_die>10+cast_time&target.time_to_die<20",
    "Rune of Power is always used with Frozen Orb. Any leftover charges at the end of the fight should be used, ideally "
    "if the boss doesn't die in the middle of the Rune buff." );
  cooldowns->add_action( "call_action_list,name=talent_rop,if=talent.rune_of_power.enabled&active_enemies=1&"
    "cooldown.rune_of_power.full_recharge_time<cooldown.frozen_orb.remains",
    "On single target fights, the cooldown of Rune of Power is lower than the cooldown of Frozen Orb, this gives "
    "extra Rune of Power charges that should be used with active talents, if possible." );
  cooldowns->add_action( "potion,if=prev_gcd.1.icy_veins|target.time_to_die<30" );
  cooldowns->add_action( "use_item,name=balefire_branch,if=!talent.glacial_spike.enabled|buff.brain_freeze.react&prev_gcd.1.glacial_spike" );
  cooldowns->add_action( "use_items" );
  for ( const auto& ra : racial_actions )
  {
    if ( ra == "arcane_torrent" )
      continue;

    cooldowns->add_action( ra );
  }

  switch ( options.rotation )
  {
    case ROTATION_STANDARD:
    case ROTATION_NO_ICE_LANCE:
      essences->add_action( "focused_azerite_beam,if=buff.rune_of_power.down|active_enemies>3" );
      essences->add_action( "memory_of_lucid_dreams,if=active_enemies<5&(buff.icicles.stack<=1|!talent.glacial_spike.enabled)&cooldown.frozen_orb.remains>10"
        + std::string( options.rotation == ROTATION_STANDARD ? "&!action.frozen_orb.in_flight&ground_aoe.frozen_orb.remains=0" : "" ) );
      essences->add_action( "blood_of_the_enemy,if=(talent.glacial_spike.enabled&buff.icicles.stack=5&(buff.brain_freeze.react|prev_gcd.1.ebonbolt))"
        "|((active_enemies>3|!talent.glacial_spike.enabled)&(prev_gcd.1.frozen_orb|ground_aoe.frozen_orb.remains>5))" );
      essences->add_action( "purifying_blast,if=buff.rune_of_power.down|active_enemies>3" );
      essences->add_action( "ripple_in_space,if=buff.rune_of_power.down|active_enemies>3" );
      essences->add_action( "concentrated_flame,line_cd=6,if=buff.rune_of_power.down" );
      essences->add_action( "the_unbound_force,if=buff.reckless_force.up" );
      essences->add_action( "worldvein_resonance,if=buff.rune_of_power.down|active_enemies>3" );
      break;
    case ROTATION_FROZEN_ORB:
      essences->add_action( "focused_azerite_beam,if=buff.rune_of_power.down&debuff.packed_ice.down|active_enemies>3" );
      essences->add_action( "memory_of_lucid_dreams,if=active_enemies<5&debuff.packed_ice.down&cooldown.frozen_orb.remains>5"
        "&!action.frozen_orb.in_flight&ground_aoe.frozen_orb.remains=0" );
      essences->add_action( "blood_of_the_enemy,if=prev_gcd.1.frozen_orb|ground_aoe.frozen_orb.remains>5" );
      essences->add_action( "purifying_blast,if=buff.rune_of_power.down&debuff.packed_ice.down|active_enemies>3" );
      essences->add_action( "ripple_in_space,if=buff.rune_of_power.down&debuff.packed_ice.down|active_enemies>3" );
      essences->add_action( "concentrated_flame,line_cd=6,if=buff.rune_of_power.down&debuff.packed_ice.down" );
      essences->add_action( "the_unbound_force,if=buff.reckless_force.up" );
      essences->add_action( "worldvein_resonance,if=buff.rune_of_power.down&debuff.packed_ice.down|active_enemies>3" );
      break;
    default:
      break;
  }

  talent_rop->add_talent( this, "Rune of Power",
    "if=talent.glacial_spike.enabled&buff.icicles.stack=5&(buff.brain_freeze.react|talent.ebonbolt.enabled&cooldown.ebonbolt.remains<cast_time)",
    "With Glacial Spike, Rune of Power should be used right before the Glacial Spike combo (i.e. with 5 Icicles and a Brain Freeze). "
    "When Ebonbolt is off cooldown, Rune of Power can also be used just with 5 Icicles." );
  talent_rop->add_talent( this, "Rune of Power",
    "if=!talent.glacial_spike.enabled&(talent.ebonbolt.enabled&cooldown.ebonbolt.remains<cast_time"
    "|talent.comet_storm.enabled&cooldown.comet_storm.remains<cast_time"
    "|talent.ray_of_frost.enabled&cooldown.ray_of_frost.remains<cast_time"
    "|charges_fractional>1.9)",
    "Without Glacial Spike, Rune of Power should be used before any bigger cooldown (Ebonbolt, Comet Storm, Ray of Frost) or "
    "when Rune of Power is about to reach 2 charges." );

  movement->add_action( "blink_any,if=movement.distance>10" );
  movement->add_talent( this, "Ice Floes", "if=buff.ice_floes.down" );
}

double mage_t::resource_regen_per_second( resource_e rt ) const
{
  double reg = player_t::resource_regen_per_second( rt );

  if ( specialization() == MAGE_ARCANE && rt == RESOURCE_MANA )
    reg *= 1.0 + cache.mastery() * spec.savant->effectN( 1 ).mastery_value();

  if ( player_t::buffs.memory_of_lucid_dreams->check() )
    reg *= 1.0 + player_t::buffs.memory_of_lucid_dreams->data().effectN( 1 ).percent();

  return reg;
}

void mage_t::invalidate_cache( cache_e c )
{
  player_t::invalidate_cache( c );

  switch ( c )
  {
    case CACHE_MASTERY:
      if ( spec.savant->ok() )
        recalculate_resource_max( RESOURCE_MANA );
      break;
    default:
      break;
  }
}

void mage_t::recalculate_resource_max( resource_e rt )
{
  double max = resources.max[ rt ];
  double pct = resources.pct( rt );

  player_t::recalculate_resource_max( rt );

  if ( specialization() == MAGE_ARCANE && rt == RESOURCE_MANA )
  {
    resources.max[ rt ] *= 1.0 + cache.mastery() * spec.savant->effectN( 1 ).mastery_value();
    resources.max[ rt ] *= 1.0 + buffs.arcane_familiar->check_value();

    resources.current[ rt ] = resources.max[ rt ] * pct;
    sim->print_debug( "{} adjusts maximum mana from {} to {} ({}%)", name(), max, resources.max[ rt ], 100 * pct );
  }
}

double mage_t::composite_player_pet_damage_multiplier( const action_state_t* s ) const
{
  double m = player_t::composite_player_pet_damage_multiplier( s );

  m *= 1.0 + spec.arcane_mage->effectN( 3 ).percent();
  m *= 1.0 + spec.fire_mage->effectN( 3 ).percent();
  m *= 1.0 + spec.frost_mage->effectN( 3 ).percent();

  m *= 1.0 + buffs.bone_chilling->check_stack_value();
  m *= 1.0 + buffs.incanters_flow->check_stack_value();
  m *= 1.0 + buffs.rune_of_power->check_value();

  return m;
}

double mage_t::composite_rating_multiplier( rating_e r ) const
{
  double rm = player_t::composite_rating_multiplier( r );

  switch ( r )
  {
    case RATING_MELEE_CRIT:
    case RATING_RANGED_CRIT:
    case RATING_SPELL_CRIT:
      rm *= 1.0 + spec.critical_mass_2->effectN( 1 ).percent();
      break;
    default:
      break;
  }

  return rm;
}

double mage_t::composite_spell_crit_chance() const
{
  double c = player_t::composite_spell_crit_chance();

  c += spec.critical_mass->effectN( 1 ).percent();

  return c;
}

double mage_t::composite_spell_haste() const
{
  double h = player_t::composite_spell_haste();

  h /= 1.0 + buffs.icy_veins->check_value();

  return h;
}

double mage_t::matching_gear_multiplier( attribute_e attr ) const
{
  if ( attr == ATTR_INTELLECT )
    return 0.05;

  return 0.0;
}

void mage_t::reset()
{
  player_t::reset();

  icicle_event = nullptr;
  ignite_spread_event = nullptr;
  time_anomaly_tick_event = nullptr;
  last_bomb_target = nullptr;
  last_frostbolt_target = nullptr;
  icicles.clear();
  ground_aoe_expiration.clear();
  burn_phase.reset();
  state = state_t();
}

void mage_t::update_movement( timespan_t duration )
{
  player_t::update_movement( duration );
  update_rune_distance( duration.total_seconds() * cache.run_speed() );
}

void mage_t::teleport( double distance, timespan_t duration )
{
  player_t::teleport( distance, duration );
  update_rune_distance( distance );
}

double mage_t::passive_movement_modifier() const
{
  double pmm = player_t::passive_movement_modifier();

  pmm += buffs.chrono_shift->check_value();
  pmm += buffs.frenetic_speed->check_value();

  return pmm;
}

void mage_t::arise()
{
  player_t::arise();

  buffs.incanters_flow->trigger();
  buffs.gbow->trigger();

  if ( spec.ignite->ok() )
  {
    timespan_t first_spread = rng().real() * spec.ignite->effectN( 3 ).period();
    ignite_spread_event = make_event<events::ignite_spread_event_t>( *sim, *this, first_spread );
  }

  if ( talents.time_anomaly->ok() )
  {
    timespan_t first_tick = rng().real() * talents.time_anomaly->effectN( 1 ).period();
    time_anomaly_tick_event = make_event<events::time_anomaly_tick_event_t>( *sim, *this, first_tick );
  }
}

void mage_t::combat_begin()
{
  player_t::combat_begin();

  if ( specialization() == MAGE_ARCANE )
  {
    // When combat starts, any Arcane Charge stacks above one are
    // removed.
    int ac_stack = buffs.arcane_charge->check();
    if ( ac_stack > 1 )
      buffs.arcane_charge->decrement( ac_stack - 1 );

    uptime.burn_phase->update( false, sim->current_time() );
    uptime.conserve_phase->update( true, sim->current_time() );
  }
}

void mage_t::combat_end()
{
  player_t::combat_end();

  if ( specialization() == MAGE_ARCANE )
  {
    uptime.burn_phase->update( false, sim->current_time() );
    uptime.conserve_phase->update( false, sim->current_time() );
  }
}

/**
 * Mage specific action expressions
 *
 * Use this function for expressions which are bound to some action property (eg. target, cast_time, etc.) and not just
 * to the player itself. For those use the normal mage_t::create_expression override.
 */
expr_t* mage_t::create_action_expression( action_t& action, const std::string& name )
{
  std::vector<std::string> splits = util::string_split( name, "." );

  // Firestarter expressions ==================================================
  if ( splits.size() == 2 && util::str_compare_ci( splits[ 0 ], "firestarter" ) )
  {
    if ( util::str_compare_ci( splits[ 1 ], "active" ) )
    {
      return make_fn_expr( name_str, [ this, &action ]
      {
        if ( !talents.firestarter->ok() )
          return false;

        if ( options.firestarter_time > 0_ms )
          return sim->current_time() < options.firestarter_time;
        else
          return action.target->health_percentage() > talents.firestarter->effectN( 1 ).base_value();
      } );
    }

    if ( util::str_compare_ci( splits[ 1 ], "remains" ) )
    {
      return make_fn_expr( name_str, [ this, &action ]
      {
        if ( !talents.firestarter->ok() )
          return 0.0;

        if ( options.firestarter_time > 0_ms )
          return std::max( options.firestarter_time - sim->current_time(), 0_ms ).total_seconds();
        else
          return action.target->time_to_percent( talents.firestarter->effectN( 1 ).base_value() ).total_seconds();
      } );
    }

    throw std::invalid_argument( fmt::format( "Unknown firestarer operation '{}'", splits[ 1 ] ) );
  }

  return player_t::create_action_expression( action, name );
}

expr_t* mage_t::create_expression( const std::string& name )
{
  // Incanters flow direction
  // Evaluates to:  0.0 if IF talent not chosen or IF stack unchanged
  //                1.0 if next IF stack increases
  //               -1.0 if IF stack decreases
  if ( util::str_compare_ci( name, "incanters_flow_dir" ) )
  {
    return make_fn_expr( name, [ this ]
    {
      if ( !talents.incanters_flow->ok() )
        return 0.0;

      if ( buffs.incanters_flow->reverse )
        return buffs.incanters_flow->check() == 1 ? 0.0 : -1.0;
      else
        return buffs.incanters_flow->check() == 5 ? 0.0 : 1.0;
    } );
  }

  if ( util::str_compare_ci( name, "burn_phase" ) )
  {
    return make_fn_expr( name, [ this ]
    { return burn_phase.on(); } );
  }

  if ( util::str_compare_ci( name, "burn_phase_duration" ) )
  {
    return make_fn_expr( name, [ this ]
    { return burn_phase.duration( sim->current_time() ).total_seconds(); } );
  }

  if ( util::str_compare_ci( name, "shooting_icicles" ) )
  {
    return make_fn_expr( name, [ this ]
    { return icicle_event != nullptr; } );
  }

  std::vector<std::string> splits = util::string_split( name, "." );

  if ( splits.size() == 3 && util::str_compare_ci( splits[ 0 ], "ground_aoe" ) )
  {
    std::string type = splits[ 1 ];
    util::tolower( type );

    if ( util::str_compare_ci( splits[ 2 ], "remains" ) )
    {
      return make_fn_expr( name, [ this, type ]
      { return std::max( ground_aoe_expiration[ type ] - sim->current_time(), 0_ms ).total_seconds(); } );
    }

    throw std::invalid_argument( fmt::format( "Unknown ground_aoe operation '{}'", splits[ 2 ] ) );
  }

  if ( splits.size() == 3 && util::str_compare_ci( splits[ 0 ], "incanters_flow_time_to" ) )
  {
    int expr_stack = std::stoi( splits[ 1 ] );
    if ( expr_stack < 1 || expr_stack > buffs.incanters_flow->max_stack() )
      throw std::invalid_argument( fmt::format( "Invalid incanters_flow_time_to stack number '{}'", splits[ 1 ] ) );

    // Number of ticks in one full cycle.
    int tick_cycle = buffs.incanters_flow->max_stack() * 2;
    int expr_pos_lo;
    int expr_pos_hi;

    if ( util::str_compare_ci( splits[ 2 ], "up" ) )
    {
      expr_pos_lo = expr_pos_hi = expr_stack;
    }
    else if ( util::str_compare_ci( splits[ 2 ], "down" ) )
    {
      expr_pos_lo = expr_pos_hi = tick_cycle - expr_stack + 1;
    }
    else if ( util::str_compare_ci( splits[ 2 ], "any" ) )
    {
      expr_pos_lo = expr_stack;
      expr_pos_hi = tick_cycle - expr_stack + 1;
    }
    else
    {
      throw std::invalid_argument( fmt::format( "Unknown incanters_flow_time_to stack type '{}'", splits[ 2 ] ) );
    }

    return make_fn_expr( name, [ this, tick_cycle, expr_pos_lo, expr_pos_hi ]
    {
      if ( !talents.incanters_flow->ok() || !buffs.incanters_flow->tick_event )
        return 0.0;

      int buff_stack = buffs.incanters_flow->check();
      int buff_pos = buffs.incanters_flow->reverse ? tick_cycle - buff_stack + 1 : buff_stack;
      if ( expr_pos_lo == buff_pos || expr_pos_hi == buff_pos )
        return 0.0;

      // Number of ticks required to reach the desired position.
      int ticks_lo = ( tick_cycle + expr_pos_lo - buff_pos ) % tick_cycle;
      int ticks_hi = ( tick_cycle + expr_pos_hi - buff_pos ) % tick_cycle;

      double tick_time = buffs.incanters_flow->tick_time().total_seconds();
      double tick_rem = buffs.incanters_flow->tick_event->remains().total_seconds();
      double value = tick_rem + tick_time * ( std::min( ticks_lo, ticks_hi ) - 1 );

      sim->print_debug( "incanters_flow_time_to: buff_position={} ticks_low={} ticks_high={} value={}",
                        buff_pos, ticks_lo, ticks_hi, value );

      return value;
    } );
  }

  return player_t::create_expression( name );
}

stat_e mage_t::convert_hybrid_stat( stat_e s ) const
{
  switch ( s )
  {
    case STAT_STR_AGI_INT:
    case STAT_AGI_INT:
    case STAT_STR_INT:
      return STAT_INTELLECT;
    case STAT_STR_AGI:
    case STAT_SPIRIT:
    case STAT_BONUS_ARMOR:
      return STAT_NONE;
    default:
      return s;
  }
}

void mage_t::vision_of_perfection_proc()
{
  if ( vision_of_perfection_multiplier <= 0.0 )
    return;

  buff_t* primary = nullptr;
  buff_t* secondary = nullptr;

  switch ( specialization() )
  {
    case MAGE_ARCANE:
      primary = buffs.arcane_power;
      break;
    case MAGE_FIRE:
      primary = buffs.combustion;
      secondary = buffs.wildfire;
      break;
    case MAGE_FROST:
      primary = buffs.icy_veins;
      secondary = buffs.frigid_grasp;
      break;
    default:
      return;
  }

  // Hotfixed to use the base duration of the buffs.
  timespan_t primary_duration = vision_of_perfection_multiplier * primary->data().duration();
  timespan_t secondary_duration = secondary ? vision_of_perfection_multiplier * secondary->data().duration() : 0_ms;

  if ( primary->check() )
  {
    primary->extend_duration( this, primary_duration );
    if ( secondary )
      secondary->extend_duration( this, secondary_duration );
  }
  else
  {
    primary->trigger( 1, buff_t::DEFAULT_VALUE(), -1.0, primary_duration );
    if ( secondary )
    {
      // For some reason, Frigid Grasp activates at a full duration.
      // TODO: This might be a bug.
      if ( specialization() == MAGE_FROST )
      {
        secondary->trigger();
      }
      else
      {
        secondary->trigger( 1, buff_t::DEFAULT_VALUE(), -1.0, secondary_duration );
      }
    }
  }
}

void mage_t::update_rune_distance( double distance )
{
  distance_from_rune += distance;

  if ( buffs.rune_of_power->check() && distance_from_rune > talents.rune_of_power->effectN( 2 ).radius() )
  {
    buffs.rune_of_power->expire();
    sim->print_debug( "{} moved out of Rune of Power.", name() );
  }
}

action_t* mage_t::get_icicle()
{
  action_t* a = nullptr;

  if ( !icicles.empty() )
  {
    event_t::cancel( icicles.front().expiration );
    a = icicles.front().action;
    icicles.erase( icicles.begin() );
  }

  return a;
}

bool mage_t::trigger_delayed_buff( buff_t* buff, double chance, timespan_t delay )
{
  bool success = rng().roll( chance );
  if ( success )
  {
    if ( buff->check() )
      make_event( sim, delay, [ buff ] { buff->trigger(); } );
    else
      buff->trigger();
  }

  return success;
}

void mage_t::trigger_brain_freeze( double chance, proc_t* source )
{
  assert( source );

  bool success = trigger_delayed_buff( buffs.brain_freeze, chance );
  if ( success )
  {
    source->occur();
    procs.brain_freeze->occur();
  }
}

void mage_t::trigger_fof( double chance, int stacks, proc_t* source )
{
  assert( source );

  bool success = buffs.fingers_of_frost->trigger( stacks, buff_t::DEFAULT_VALUE(), chance );
  if ( success )
  {
    if ( chance >= 1.0 )
      buffs.fingers_of_frost->predict();

    for ( int i = 0; i < stacks; i++ )
    {
      source->occur();
      procs.fingers_of_frost->occur();
    }
  }
}

void mage_t::trigger_icicle( player_t* icicle_target, bool chain )
{
  assert( icicle_target );

  if ( !spec.icicles->ok() )
    return;

  if ( icicles.empty() )
    return;

  if ( chain && !icicle_event )
  {
    icicle_event = make_event<events::icicle_event_t>( *sim, *this, icicle_target, true );
    sim->print_debug( "{} icicle use on {} (chained), total={}", name(), icicle_target->name(), icicles.size() );
  }
  else if ( !chain )
  {
    action_t* icicle_action = get_icicle();
    icicle_action->set_target( icicle_target );
    icicle_action->execute();
    sim->print_debug( "{} icicle use on {}, total={}", name(), icicle_target->name(), icicles.size() );
  }
}

void mage_t::trigger_icicle_gain( player_t* icicle_target, action_t* icicle_action )
{
  if ( !spec.icicles->ok() )
    return;

  unsigned max_icicles = as<unsigned>( spec.icicles->effectN( 2 ).base_value() );

  // Shoot one if capped
  if ( icicles.size() == max_icicles )
    trigger_icicle( icicle_target );

  buffs.icicles->trigger();
  icicles.push_back( { icicle_action, make_event( sim, buffs.icicles->buff_duration, [ this ]
  {
    buffs.icicles->decrement();
    icicles.erase( icicles.begin() );
  } ) } );

  assert( icicle_action && icicles.size() <= max_icicles );
}

void mage_t::trigger_evocation( timespan_t duration_override, bool hasted )
{
  double mana_regen_multiplier = 1.0 + buffs.evocation->default_value;

  timespan_t duration = duration_override;
  if ( duration <= 0_ms )
    duration = buffs.evocation->buff_duration;

  if ( hasted )
  {
    mana_regen_multiplier /= cache.spell_speed();
    duration *= cache.spell_speed();
  }

  buffs.evocation->trigger( 1, mana_regen_multiplier, -1.0, duration );
}

void mage_t::trigger_arcane_charge( int stacks )
{
  buff_t* ac = buffs.arcane_charge;

  int before = ac->check();
  ac->trigger( stacks );
  int after = ac->check();

  if ( before < 3 && after >= 3 )
    buffs.rule_of_threes->trigger();
}

void mage_t::trigger_leyshock( unsigned id, const action_state_t*, leyshock_trigger_e trigger_type )
{
  if ( !player_t::buffs.leyshock_crit )
    return;

  stat_e buff = STAT_NONE;

  switch ( trigger_type )
  {
    case LEYSHOCK_EXECUTE:
      switch ( id )
      {
        case 120: // Cone of Cold
        case 12472: // Icy Veins
        case 190356: // Blizard
        case 228354: // Flurry tick
          buff = STAT_CRIT_RATING;
          break;
        case 1953: // Blink
        case 55342: // Mirror Image
        case 84721: // Frozen Orb tick
        case 153596: // Comet Storm tick
        case 157997: // Ice Nova
        case 190357: // Blizzard tick
        case 205021: // Ray of Frost
        case 212653: // Shimmer
          buff = STAT_HASTE_RATING;
          break;
        case 1459: // Arcane Intellect
        case 2139: // Counterspell
        case 30455: // Ice Lance
        case 31687: // Summon Water Elemental
        case 108839: // Ice Floes
        case 116011: // Rune of Power
        case 153595: // Comet Storm
        case 235219: // Cold Snap
          buff = STAT_VERSATILITY_RATING;
          break;
        case 122: // Frost Nova
        case 80353: // Time Warp
        case 84714: // Frozen Orb
        case 148022: // Icicle
        case 199786: // Glacial Spike
        case 257537: // Ebonbolt
          buff = STAT_MASTERY_RATING;
          break;
        case 116: // Frostbolt
        case 44614: // Flurry
          switch ( buffs.icicles->check() )
          {
            case 4:
            case 5:
              buff = STAT_CRIT_RATING;
              break;
            case 3:
              buff = STAT_HASTE_RATING;
              break;
            case 1:
              buff = STAT_VERSATILITY_RATING;
              break;
            case 0:
            case 2:
              buff = STAT_MASTERY_RATING;
              break;
            default:
              break;
          }
          break;
        default:
          break;
      }
      break;
    case LEYSHOCK_IMPACT:
      switch ( id )
      {
        case 84714: // Frozen Orb
        case 153596: // Comet Storm tick
        case 199786: // Glacial Spike
          buff = STAT_CRIT_RATING;
          break;
        case 116: // Frostbolt
          buff = STAT_HASTE_RATING;
          break;
        case 30455: // Ice Lance
        case 228354: // Flurry tick
          buff = STAT_MASTERY_RATING;
          break;
        default:
          break;
      }
      break;
    case LEYSHOCK_TICK:
      switch ( id )
      {
        case 205021: // Ray of Frost
          buff = STAT_HASTE_RATING;
          break;
        default:
          break;
      }
      break;
    case LEYSHOCK_BUMP:
      switch ( id )
      {
        case 116267: // Incanter's Flow
          buff = STAT_MASTERY_RATING;
          break;
        default:
          break;
      }
    default:
      break;
  }

  expansion::bfa::trigger_leyshocks_grand_compilation( buff, this );
}

void mage_t::trigger_lucid_dreams( player_t* trigger_target, double cost )
{
  if ( lucid_dreams_refund <= 0.0 )
    return;

  if ( cost <= 0.0 )
    return;

  double proc_chance =
    ( specialization() == MAGE_ARCANE ) ? options.lucid_dreams_proc_chance_arcane :
    ( specialization() == MAGE_FIRE   ) ? options.lucid_dreams_proc_chance_fire :
                                          options.lucid_dreams_proc_chance_frost;

  if ( rng().roll( proc_chance ) )
  {
    switch ( specialization() )
    {
      case MAGE_ARCANE:
        resource_gain( RESOURCE_MANA, lucid_dreams_refund * cost, gains.lucid_dreams );
        break;
      case MAGE_FIRE:
        cooldowns.fire_blast->adjust( -lucid_dreams_refund * cooldown_t::cooldown_duration( cooldowns.fire_blast ) );
        break;
      case MAGE_FROST:
        trigger_icicle_gain( trigger_target, icicle.lucid_dreams );
        break;
      default:
        break;
    }

    player_t::buffs.lucid_dreams->trigger();
  }
}

/* Report Extension Class
 * Here you can define class specific report extensions/overrides
 */
class mage_report_t : public player_report_extension_t
{
public:
  mage_report_t( mage_t& player ) :
    p( player )
  { }

  void html_customsection_cd_waste( report::sc_html_stream& os )
  {
    if ( p.cooldown_waste_data_list.empty() )
      return;

    os << "<div class=\"player-section custom_section\">\n"
          "<h3 class=\"toggle open\">Cooldown waste</h3>\n"
          "<div class=\"toggle-content\">\n"
          "<table class=\"sc sort even\">\n"
          "<thead>\n"
          "<tr>\n"
          "<th></th>\n"
          "<th colspan=\"3\">Seconds per Execute</th>\n"
          "<th colspan=\"3\">Seconds per Iteration</th>\n"
          "</tr>\n"
          "<tr>\n"
          "<th class=\"toggle-sort\" data-sortdir=\"asc\" data-sorttype=\"alpha\">Ability</th>\n"
          "<th class=\"toggle-sort\">Average</th>\n"
          "<th class=\"toggle-sort\">Minimum</th>\n"
          "<th class=\"toggle-sort\">Maximum</th>\n"
          "<th class=\"toggle-sort\">Average</th>\n"
          "<th class=\"toggle-sort\">Minimum</th>\n"
          "<th class=\"toggle-sort\">Maximum</th>\n"
          "</tr>\n"
          "</thead>\n";

    for ( const cooldown_waste_data_t* data : p.cooldown_waste_data_list )
    {
      if ( !data->active() )
        continue;

      std::string name = data->cd->name_str;
      if ( action_t* a = p.find_action( name ) )
        name = report::action_decorator_t( a ).decorate();
      else
        name = util::encode_html( name );

      os << "<tr>";
      fmt::print( os, "<td class=\"left\">{}</td>", name );
      fmt::print( os, "<td class=\"right\">{:.3f}</td>", data->normal.mean() );
      fmt::print( os, "<td class=\"right\">{:.3f}</td>", data->normal.min() );
      fmt::print( os, "<td class=\"right\">{:.3f}</td>", data->normal.max() );
      fmt::print( os, "<td class=\"right\">{:.3f}</td>", data->cumulative.mean() );
      fmt::print( os, "<td class=\"right\">{:.3f}</td>", data->cumulative.min() );
      fmt::print( os, "<td class=\"right\">{:.3f}</td>", data->cumulative.max() );
      os << "</tr>\n";
    }

    os << "</table>\n"
          "</div>\n"
          "</div>\n";
  }

  void html_customsection_burn_phases( report::sc_html_stream& os )
  {
    os << "<div class=\"player-section custom_section\">\n"
          "<h3 class=\"toggle open\">Burn Phases</h3>\n"
          "<div class=\"toggle-content\">\n"
          "<p>Burn phase duration tracks the amount of time spent in each burn phase. This is defined as the time between a "
          "start_burn_phase and stop_burn_phase action being executed. Note that \"execute\" burn phases, i.e., the "
          "final burn of a fight, is also included.</p>\n"
          "<div class=\"flexwrap\">\n"
          "<table class=\"sc even\">\n"
          "<thead>\n"
          "<tr>\n"
          "<th>Burn Phase Duration</th>\n"
          "</tr>\n"
          "</thead>\n"
          "<tbody>\n";

    auto print_sample_data = [ &os ] ( extended_sample_data_t& s )
    {
      fmt::print( os, "<tr><td class=\"left\">Count</td><td>{}</td></tr>\n", s.count() );
      fmt::print( os, "<tr><td class=\"left\">Minimum</td><td>{:.3f}</td></tr>\n", s.min() );
      fmt::print( os, "<tr><td class=\"left\">5<sup>th</sup> percentile</td><td>{:.3f}</td></tr>\n", s.percentile( 0.05 ) );
      fmt::print( os, "<tr><td class=\"left\">Mean</td><td>{:.3f}</td></tr>\n", s.mean() );
      fmt::print( os, "<tr><td class=\"left\">95<sup>th</sup> percentile</td><td>{:.3f}</td></tr>\n", s.percentile( 0.95 ) );
      fmt::print( os, "<tr><td class=\"left\">Max</td><td>{:.3f}</td></tr>\n", s.max() );
      fmt::print( os, "<tr><td class=\"left\">Variance</td><td>{:.3f}</td></tr>\n", s.variance );
      fmt::print( os, "<tr><td class=\"left\">Mean Variance</td><td>{:.3f}</td></tr>\n", s.mean_variance );
      fmt::print( os, "<tr><td class=\"left\">Mean Std. Dev</td><td>{:.3f}</td></tr>\n", s.mean_std_dev );
    };

    print_sample_data( *p.sample_data.burn_duration_history );

    os << "</tbody>\n"
          "</table>\n";

    auto& h = *p.sample_data.burn_duration_history;
    highchart::histogram_chart_t chart( highchart::build_id( p, "burn_duration" ), *p.sim );
    if ( chart::generate_distribution( chart, &p, h.distribution, "Burn Duration", h.mean(), h.min(), h.max() ) )
    {
      chart.set( "tooltip.headerFormat", "<b>{point.key}</b> s<br/>" );
      chart.set( "chart.width", "575" );
      os << chart.to_target_div();
      p.sim->add_chart_data( chart );
    }

    os << "</div>\n"
          "<p>Mana at burn start is the mana level recorded (in percentage of total mana) when a start_burn_phase command is executed.</p>\n"
          "<table class=\"sc even\">\n"
          "<thead>\n"
          "<tr>\n"
          "<th>Mana at Burn Start</th>\n"
          "</tr>\n"
          "</thead>\n"
          "<tbody>\n";

    print_sample_data( *p.sample_data.burn_initial_mana );

    os << "</tbody>\n"
          "</table>\n"
          "</div>\n"
          "</div>\n";
  }

  void html_customsection_icy_veins( report::sc_html_stream& os )
  {
    os << "<div class=\"player-section custom_section\">\n"
          "<h3 class=\"toggle open\">Icy Veins</h3>\n"
          "<div class=\"toggle-content\">\n";

    auto& d = *p.sample_data.icy_veins_duration;
    int num_buckets = std::min( 70, as<int>( d.max() - d.min() ) + 1 );
    d.create_histogram( num_buckets );

    highchart::histogram_chart_t chart( highchart::build_id( p, "icy_veins_duration" ), *p.sim );
    if ( chart::generate_distribution( chart, &p, d.distribution, "Icy Veins Duration", d.mean(), d.min(), d.max() ) )
    {
      chart.set( "tooltip.headerFormat", "<b>{point.key}</b> s<br/>" );
      chart.set( "chart.width", std::to_string( 80 + num_buckets * 13 ) );
      os << chart.to_target_div();
      p.sim->add_chart_data( chart );
    }

    os << "</div>\n"
          "</div>\n";
  }

  void html_customsection_shatter( report::sc_html_stream& os )
  {
    if ( p.shatter_source_list.empty() )
      return;

    os << "<div class=\"player-section custom_section\">\n"
          "<h3 class=\"toggle open\">Shatter</h3>\n"
          "<div class=\"toggle-content\">\n"
          "<table class=\"sc sort even\">\n"
          "<thead>\n"
          "<tr>\n"
          "<th></th>\n"
          "<th colspan=\"2\">None</th>\n"
          "<th colspan=\"3\">Winter's Chill</th>\n"
          "<th colspan=\"2\">Fingers of Frost</th>\n"
          "<th colspan=\"2\">Other effects</th>\n"
          "</tr>\n"
          "<tr>\n"
          "<th class=\"toggle-sort\" data-sortdir=\"asc\" data-sorttype=\"alpha\">Ability</th>\n"
          "<th class=\"toggle-sort\">Count</th>\n"
          "<th class=\"toggle-sort\">Percent</th>\n"
          "<th class=\"toggle-sort\">Count</th>\n"
          "<th class=\"toggle-sort\">Percent</th>\n"
          "<th class=\"toggle-sort\">Utilization</th>\n"
          "<th class=\"toggle-sort\">Count</th>\n"
          "<th class=\"toggle-sort\">Percent</th>\n"
          "<th class=\"toggle-sort\">Count</th>\n"
          "<th class=\"toggle-sort\">Percent</th>\n"
          "</tr>\n"
          "</thead>\n";

    double bff = p.procs.brain_freeze_used->count.pretty_mean();

    for ( const shatter_source_t* data : p.shatter_source_list )
    {
      if ( !data->active() )
        continue;

      auto nonzero = [] ( std::string fmt, double d ) { return d != 0.0 ? fmt::format( fmt, d ) : ""; };
      auto cells = [ &, total = data->count_total() ] ( double mean, bool util = false )
      {
        std::string format_str = "<td class=\"right\">{}</td><td class=\"right\">{}</td>";
        if ( util ) format_str += "<td class=\"right\">{}</td>";

        fmt::print( os, format_str,
          nonzero( "{:.1f}", mean ),
          nonzero( "{:.1f}%", 100.0 * mean / total ),
          nonzero( "{:.1f}%", bff > 0.0 ? 100.0 * mean / bff : 0.0 ) );
      };

      std::string name = data->name_str;
      if ( action_t* a = p.find_action( name ) )
        name = report::action_decorator_t( a ).decorate();
      else
        name = util::encode_html( name );

      os << "<tr>";
      fmt::print( os, "<td class=\"left\">{}</td>", name );
      cells( data->count( FROZEN_NONE ) );
      cells( data->count( FROZEN_WINTERS_CHILL ), true );
      cells( data->count( FROZEN_FINGERS_OF_FROST ) );
      cells( data->count( FROZEN_ROOT ) );
      os << "</tr>\n";
    }

    os << "</table>\n"
          "</div>\n"
          "</div>\n";
  }

  void html_customsection( report::sc_html_stream& os ) override
  {
    if ( p.sim->report_details == 0 )
      return;

    html_customsection_cd_waste( os );
    switch ( p.specialization() )
    {
      case MAGE_ARCANE:
        html_customsection_burn_phases( os );
        break;
      case MAGE_FROST:
        html_customsection_shatter( os );
        if ( p.talents.thermal_void->ok() )
          html_customsection_icy_veins( os );
        break;
      default:
        break;
    }
  }
private:
  mage_t& p;
};

// MAGE MODULE INTERFACE ====================================================

struct mage_module_t : public module_t
{
public:
  mage_module_t() :
    module_t( MAGE )
  { }

  player_t* create_player( sim_t* sim, const std::string& name, race_e r = RACE_NONE ) const override
  {
    auto p = new mage_t( sim, name, r );
    p->report_extension = std::make_unique<mage_report_t>( *p );
    return p;
  }

  void register_hotfixes() const override
  {
    hotfix::register_spell( "Mage", "2018-05-02", "Incorrect spell level for Icicle buff.", 205473 )
      .field( "spell_level" )
      .operation( hotfix::HOTFIX_SET )
      .modifier( 78 )
      .verification_value( 80 );

    hotfix::register_spell( "Mage", "2017-11-06", "Incorrect spell level for Icicle.", 148022 )
      .field( "spell_level" )
      .operation( hotfix::HOTFIX_SET )
      .modifier( 78 )
      .verification_value( 80 );

    hotfix::register_spell( "Mage", "2017-11-08", "Incorrect spell level for Ignite.", 12654 )
      .field( "spell_level" )
      .operation( hotfix::HOTFIX_SET )
      .modifier( 78 )
      .verification_value( 99 );

    hotfix::register_spell( "Mage", "2017-03-20", "Manually set Frozen Orb's travel speed.", 84714 )
      .field( "prj_speed" )
      .operation( hotfix::HOTFIX_SET )
      .modifier( 20.0 )
      .verification_value( 0.0 );

    hotfix::register_spell( "Mage", "2017-06-21", "Ice Lance is slower than spell data suggests.", 30455 )
      .field( "prj_speed" )
      .operation( hotfix::HOTFIX_SET )
      .modifier( 47.0 )
      .verification_value( 50.0 );

    hotfix::register_spell( "Mage", "2018-12-28", "Manually set Arcane Orb's travel speed.", 153626 )
      .field( "prj_speed" )
      .operation( hotfix::HOTFIX_SET )
      .modifier( 20.0 )
      .verification_value( 0.0 );
  }

  bool valid() const override { return true; }
  void init( player_t* ) const override {}
  void combat_begin( sim_t* ) const override {}
  void combat_end( sim_t* ) const override {}
};

}  // UNNAMED NAMESPACE

const module_t* module_t::mage()
{
  static mage_module_t m;
  return &m;
}
