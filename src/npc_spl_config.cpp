#include "npc_spl_config.h"

#include <imgui/imgui.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <map>
#include <sstream>
#include <utility>

#include "cata_imgui.h"
#include "character.h"
#include "color.h"
#include "debug.h"
#include "dialogue.h"
#include "flexbuffer_json.h"
#include "input.h"
#include "input_context.h"
#include "json.h"
#include "json_error.h"
#include "json_loader.h"
#include "math_parser_diag_value.h"
#include "mission.h"
#include "npc.h"
#include "rng.h"
#include "string_formatter.h"
#include "string_input_popup.h"
#include "translations.h"
#include "ui_manager.h"
#include "uilist.h"
#include "units.h"

static const bionic_id bio_armor_torso( "bio_armor_torso" );
static const bionic_id bio_flashlight( "bio_flashlight" );
static const bionic_id bio_power_storage( "bio_power_storage" );

static const npc_class_id NC_DOCTOR( "NC_DOCTOR" );
static const npc_class_id NC_JUNK_SHOPKEEP( "NC_JUNK_SHOPKEEP" );
static const npc_class_id NC_NONE( "NC_NONE" );
static const npc_class_id NC_SCAVENGER( "NC_SCAVENGER" );

static const skill_id skill_firstaid( "firstaid" );
static const skill_id skill_melee( "melee" );
static const skill_id skill_survival( "survival" );

namespace npc_spl
{
static const std::string spl_value_key = "npc_spl_config";
static const std::string topic_prefix = "TALK_SPL_FIRST_NPC_";
static const std::string default_npc_name = "Alex";

std::string foda_symbol( foda_kind kind )
{
    switch( kind ) {
        case foda_kind::mandatory:
            return "[●]";
        case foda_kind::optional:
            return "[○]";
        case foda_kind::alternative:
            return "[△]";
        case foda_kind::or_group:
            return "[▲]";
    }
    return "[?]";
}

const std::vector<group_info> &all_groups()
{
    static const std::vector<group_info> groups = {
        { feature_group::attitude, _( "Atitude" ), foda_kind::mandatory, foda_kind::alternative },
        { feature_group::archetype, _( "Classe/Arquétipo" ), foda_kind::mandatory, foda_kind::alternative },
        { feature_group::roles, _( "Papéis Funcionais" ), foda_kind::optional, foda_kind::or_group },
        { feature_group::modifiers, _( "Modificadores" ), foda_kind::optional, foda_kind::optional },
    };
    return groups;
}

const std::vector<feature_info> &all_features()
{
    static const std::vector<feature_info> features = {
        {
            feature::att_friendly, feature_group::attitude, "att_friendly", _( "Amigável" ),
            _( "Recebe você de braços abertos e confia em você desde o início." )
        },
        {
            feature::att_neutral, feature_group::attitude, "att_neutral", _( "Neutro" ),
            _( "Educado, mas reservado.  Precisa ser convencido para cooperar." )
        },
        {
            feature::att_cautious, feature_group::attitude, "att_cautious", _( "Cauteloso" ),
            _( "Desconfiado.  Testes de persuasão ficam mais difíceis." )
        },
        {
            feature::att_hostile, feature_group::attitude, "att_hostile", _( "Hostil" ),
            _( "Ameaça você e ataca se você não for embora.  EXCLUI todos os papéis funcionais." )
        },
        {
            feature::arch_survivor, feature_group::archetype, "arch_survivor", _( "Sobrevivente" ),
            _( "Classe NC_NONE: um evacuado comum." )
        },
        {
            feature::arch_merchant, feature_group::archetype, "arch_merchant", _( "Mercador" ),
            _( "Classe NC_JUNK_SHOPKEEP, com estoque de loja.  REQUER o papel Comerciante." )
        },
        {
            feature::arch_medic, feature_group::archetype, "arch_medic", _( "Médico" ),
            _( "Classe NC_DOCTOR: habilidades médicas e suprimentos." )
        },
        {
            feature::arch_raider, feature_group::archetype, "arch_raider", _( "Saqueador" ),
            _( "Classe NC_SCAVENGER: mais agressivo e acostumado a saquear." )
        },
        {
            feature::role_trader, feature_group::roles, "role_trader", _( "Comerciante (Escambo)" ),
            _( "INCLUI o tópico de escambo (start_trade) na árvore de diálogos." )
        },
        {
            feature::role_quest_giver, feature_group::roles, "role_quest_giver", _( "Dador de Missões" ),
            _( "O NPC recebe uma missão inicial e INCLUI o link para TALK_MISSION_LIST." )
        },
        {
            feature::role_recruitable, feature_group::roles, "role_recruitable", _( "Recrutável (Seguidor)" ),
            _( "INCLUI o tópico de recrutamento (efeito follow), com teste de persuasão conforme a atitude." )
        },
        {
            feature::role_instructor, feature_group::roles, "role_instructor", _( "Instrutor" ),
            _( "Aumenta perícias de ensino e INCLUI o link para TALK_TRAIN." )
        },
        {
            feature::mod_bionics, feature_group::modifiers, "mod_bionics", _( "Possui Biônicos (CBMs)" ),
            _( "Instala CBMs (armazenamento de energia, lanterna, blindagem do torso) e INCLUI um tópico sobre eles." )
        },
        {
            feature::mod_local_lore, feature_group::modifiers, "mod_local_lore", _( "Conhece Lore Local" ),
            _( "INCLUI um tópico sobre a cidade mais próxima (tag <city>)." )
        },
    };
    return features;
}

const std::vector<cross_tree_constraint> &all_constraints()
{
    static const std::vector<cross_tree_constraint> constraints = {
        { feature::att_hostile, constraint_type::excludes_feature, feature::role_trader },
        { feature::att_hostile, constraint_type::excludes_feature, feature::role_quest_giver },
        { feature::att_hostile, constraint_type::excludes_feature, feature::role_recruitable },
        { feature::att_hostile, constraint_type::excludes_feature, feature::role_instructor },
        { feature::arch_merchant, constraint_type::requires_feature, feature::role_trader },
    };
    return constraints;
}

const feature_info &info( feature f )
{
    return all_features()[static_cast<std::size_t>( f )];
}

const group_info &info( feature_group g )
{
    return all_groups()[static_cast<std::size_t>( g )];
}

static std::vector<feature> features_of( feature_group g )
{
    std::vector<feature> result;
    for( const feature_info &fi : all_features() ) {
        if( fi.group == g ) {
            result.push_back( fi.id );
        }
    }
    return result;
}

static std::size_t idx( feature f )
{
    return static_cast<std::size_t>( f );
}

npc_feature_config::npc_feature_config() : name( default_npc_name )
{
    selected.set( idx( feature::att_friendly ) );
    selected.set( idx( feature::arch_survivor ) );
    validate_and_propagate_constraints();
}

bool npc_feature_config::is_selected( feature f ) const
{
    return selected.test( idx( f ) );
}

bool npc_feature_config::is_blocked( feature f ) const
{
    return blocked.test( idx( f ) );
}

bool npc_feature_config::is_forced( feature f ) const
{
    return forced.test( idx( f ) );
}

feature npc_feature_config::selected_in( feature_group g ) const
{
    for( feature f : features_of( g ) ) {
        if( is_selected( f ) ) {
            return f;
        }
    }
    return feature::num_features;
}

std::string npc_feature_config::constraint_reason( feature f ) const
{
    for( const cross_tree_constraint &c : all_constraints() ) {
        if( c.type == constraint_type::excludes_feature && c.to == f && is_selected( c.from ) ) {
            return string_format( _( "%s EXCLUI %s" ), info( c.from ).label, info( f ).label );
        }
    }
    for( const cross_tree_constraint &c : all_constraints() ) {
        if( c.type == constraint_type::requires_feature && c.from == f && is_blocked( c.to ) ) {
            return string_format( _( "%s REQUER %s, que está bloqueada (%s)" ), info( f ).label,
                                  info( c.to ).label, constraint_reason( c.to ) );
        }
    }
    for( const cross_tree_constraint &c : all_constraints() ) {
        if( c.type == constraint_type::requires_feature && c.to == f && is_selected( c.from ) ) {
            return string_format( _( "%s REQUER %s" ), info( c.from ).label, info( f ).label );
        }
    }
    return std::string();
}

bool npc_feature_config::choose( feature f )
{
    if( is_blocked( f ) ) {
        return false;
    }
    const group_info &g = info( info( f ).group );
    if( g.decomposition == foda_kind::alternative ) {
        for( feature sibling : features_of( g.id ) ) {
            selected.reset( idx( sibling ) );
        }
        selected.set( idx( f ) );
    } else {
        if( is_selected( f ) && is_forced( f ) ) {
            return false;
        }
        selected.flip( idx( f ) );
    }
    validate_and_propagate_constraints();
    return true;
}

std::vector<std::string> npc_feature_config::validate_and_propagate_constraints()
{
    propagation_log.clear();
    constexpr int max_iterations = 16;
    for( int iteration = 0; iteration < max_iterations; ++iteration ) {
        bool changed = false;
        blocked.reset();
        forced.reset();

        for( const cross_tree_constraint &c : all_constraints() ) {
            if( c.type == constraint_type::excludes_feature && is_selected( c.from ) ) {
                blocked.set( idx( c.to ) );
            }
        }
        for( bool grew = true; grew; ) {
            grew = false;
            for( const cross_tree_constraint &c : all_constraints() ) {
                if( c.type == constraint_type::requires_feature && is_blocked( c.to ) &&
                    !is_blocked( c.from ) ) {
                    blocked.set( idx( c.from ) );
                    grew = true;
                }
            }
        }
        for( const feature_info &fi : all_features() ) {
            if( is_selected( fi.id ) && is_blocked( fi.id ) ) {
                propagation_log.push_back( string_format( _( "%s desmarcada [%s]" ), fi.label,
                                           constraint_reason( fi.id ) ) );
                selected.reset( idx( fi.id ) );
                changed = true;
            }
        }
        for( const cross_tree_constraint &c : all_constraints() ) {
            if( c.type == constraint_type::requires_feature && is_selected( c.from ) &&
                !is_blocked( c.to ) ) {
                forced.set( idx( c.to ) );
                if( !is_selected( c.to ) ) {
                    selected.set( idx( c.to ) );
                    propagation_log.push_back( string_format( _( "%s marcada automaticamente [%s]" ),
                                               info( c.to ).label, constraint_reason( c.to ) ) );
                    changed = true;
                }
            }
        }
        for( const group_info &g : all_groups() ) {
            if( g.decomposition != foda_kind::alternative ) {
                continue;
            }
            std::vector<feature> chosen;
            std::optional<feature> first_free;
            for( feature f : features_of( g.id ) ) {
                if( is_selected( f ) ) {
                    chosen.push_back( f );
                }
                if( !first_free && !is_blocked( f ) ) {
                    first_free = f;
                }
            }
            if( chosen.size() > 1 ) {
                for( auto it = std::next( chosen.begin() ); it != chosen.end(); ++it ) {
                    selected.reset( idx( *it ) );
                }
                propagation_log.push_back( string_format( _( "%s: XOR violado, mantida apenas %s" ),
                                           g.label, info( chosen.front() ).label ) );
                changed = true;
            } else if( chosen.empty() && first_free ) {
                selected.set( idx( *first_free ) );
                propagation_log.push_back( string_format( _( "%s: XOR sem escolha, selecionada %s" ),
                                           g.label, info( *first_free ).label ) );
                changed = true;
            }
        }
        if( !changed ) {
            break;
        }
    }
    if( name.empty() ) {
        name = default_npc_name;
        propagation_log.push_back( string_format( _( "Nome é obrigatório: usando \"%s\"" ), name ) );
    }
    return propagation_log;
}

bool npc_feature_config::is_valid( std::string *why ) const
{
    const auto fail = [why]( const std::string &msg ) {
        if( why != nullptr ) {
            *why = msg;
        }
        return false;
    };
    if( name.empty() ) {
        return fail( _( "Identidade/Nome é obrigatória." ) );
    }
    for( const group_info &g : all_groups() ) {
        if( g.decomposition != foda_kind::alternative ) {
            continue;
        }
        const std::vector<feature> members = features_of( g.id );
        const auto count = std::count_if( members.begin(), members.end(), [this]( feature f ) {
            return is_selected( f );
        } );
        if( count != 1 ) {
            return fail( string_format( _( "%s exige exatamente uma escolha." ), g.label ) );
        }
    }
    for( const cross_tree_constraint &c : all_constraints() ) {
        if( !is_selected( c.from ) ) {
            continue;
        }
        if( c.type == constraint_type::excludes_feature && is_selected( c.to ) ) {
            return fail( string_format( _( "%s EXCLUI %s." ), info( c.from ).label, info( c.to ).label ) );
        }
        if( c.type == constraint_type::requires_feature && !is_selected( c.to ) ) {
            return fail( string_format( _( "%s REQUER %s." ), info( c.from ).label, info( c.to ).label ) );
        }
    }
    return true;
}

void npc_feature_config::serialize( JsonOut &jsout ) const
{
    jsout.start_object();
    jsout.member( "name", name );
    jsout.member( "features" );
    jsout.start_array();
    for( const feature_info &fi : all_features() ) {
        if( is_selected( fi.id ) ) {
            jsout.write( fi.str_id );
        }
    }
    jsout.end_array();
    jsout.end_object();
}

void npc_feature_config::deserialize( const JsonObject &jo )
{
    jo.allow_omitted_members();
    name = jo.get_string( "name", default_npc_name );
    selected.reset();
    for( const std::string &str_id : jo.get_string_array( "features" ) ) {
        const auto &features = all_features();
        const auto it = std::find_if( features.begin(), features.end(),
        [&str_id]( const feature_info & fi ) {
            return fi.str_id == str_id;
        } );
        if( it != features.end() ) {
            selected.set( idx( it->id ) );
        }
    }
    validate_and_propagate_constraints();
}

std::string npc_feature_config::to_json_string() const
{
    std::ostringstream os;
    JsonOut jsout( os );
    serialize( jsout );
    return os.str();
}

std::optional<npc_feature_config> npc_feature_config::from_json_string( const std::string &data )
{
    try {
        JsonValue jv = json_loader::from_string( data );
        npc_feature_config cfg;
        cfg.deserialize( jv.get_object() );
        return cfg;
    } catch( const JsonError &e ) {
        debugmsg( "Invalid NPC SPL configuration: %s", e.what() );
    }
    return std::nullopt;
}

static std::string topic( const std::string &suffix )
{
    return topic_prefix + suffix;
}

const std::vector<std::string> &all_dialogue_topic_ids()
{
    static const std::vector<std::string> ids = {
        topic( "ROOT" ), topic( "THREAT" ), topic( "THREAT_CALM" ), topic( "IDENTITY" ),
        topic( "BARTER" ), topic( "RECRUIT" ), topic( "RECRUIT_ACCEPT" ), topic( "RECRUIT_REFUSE" ),
        topic( "LORE" ), topic( "CBM" ), topic( "SITUATION" )
    };
    return ids;
}

static dialogue_response_node reply( const std::string &text, const std::string &target,
                                     const std::string &effect = std::string() )
{
    dialogue_response_node r;
    r.text = text;
    r.topic = target;
    r.effect = effect;
    return r;
}

static dialogue_response_node persuade( const std::string &text, int difficulty,
                                        const std::string &success_topic, const std::string &success_effect,
                                        const std::string &failure_topic, const std::string &failure_effect = std::string() )
{
    dialogue_response_node r = reply( text, success_topic, success_effect );
    r.persuade_difficulty = difficulty;
    r.failure_topic = failure_topic;
    r.failure_effect = failure_effect;
    return r;
}

static std::string archetype_story( feature arch )
{
    switch( arch ) {
        case feature::arch_merchant:
            return _( "Eu tinha uma lojinha no centro.  Ainda sei o valor de cada parafuso, e o "
                      "dinheiro não vale mais nada: agora é tudo na base da troca." );
        case feature::arch_medic:
            return _( "Eu trabalhava no pronto-socorro.  Sei fechar um corte e tratar uma "
                      "infecção, se tivermos material." );
        case feature::arch_raider:
            return _( "Digamos que eu pego o que ninguém está usando.  Os mortos não reclamam." );
        case feature::arch_survivor:
        default:
            return _( "Ninguém especial.  Só alguém que teve a sorte de chegar vivo a este abrigo." );
    }
}

dialogue_feature_model derive_dialogue_tree( const npc_feature_config &cfg )
{
    dialogue_feature_model model;

    const feature att = cfg.selected_in( feature_group::attitude );
    const bool hostile = att == feature::att_hostile;
    const bool trader = cfg.is_selected( feature::role_trader );
    const bool quest_giver = cfg.is_selected( feature::role_quest_giver );
    const bool recruitable = cfg.is_selected( feature::role_recruitable );
    const bool instructor = cfg.is_selected( feature::role_instructor );
    const bool cbm = cfg.is_selected( feature::mod_bionics );
    const bool lore = cfg.is_selected( feature::mod_local_lore );

    const auto include = [&model]( bool presence, dialogue_topic_node node ) {
        if( presence ) {
            model.topics.push_back( std::move( node ) );
        } else {
            model.pruned.push_back( { node.id, node.presence_condition } );
        }
    };
    const auto node = []( const std::string & id, foda_kind kind, const std::string & presence,
    const std::string & line ) {
        dialogue_topic_node n;
        n.id = id;
        n.kind = kind;
        n.presence_condition = presence;
        n.dynamic_line = line;
        return n;
    };

    std::vector<dialogue_response_node> modifier_links;
    if( lore ) {
        modifier_links.push_back( reply( _( "O que você sabe sobre esta região?" ), topic( "LORE" ) ) );
    }
    if( cbm ) {
        modifier_links.push_back( reply( _( "Tem alguma coisa brilhando debaixo da sua pele…" ),
                                         topic( "CBM" ) ) );
    }

    {
        std::string line;
        switch( att ) {
            case feature::att_friendly:
                line = string_format( _( "Graças a Deus, outro rosto vivo!  Meu nome é %s.  "
                                         "Pode contar comigo." ), cfg.name );
                break;
            case feature::att_cautious:
                line = string_format( _( "Fique onde está…  Tá, você não parece um deles.  "
                                         "Sou %s.  O que você quer?" ), cfg.name );
                break;
            case feature::att_neutral:
            default:
                line = string_format( _( "Sou %s.  Estamos presos aqui juntos, então vamos "
                                         "manter as coisas civilizadas." ), cfg.name );
                break;
        }
        dialogue_topic_node root = node( topic( "ROOT" ), foda_kind::alternative,
                                         _( "Atitude ≠ Hostil" ), line );
        root.responses.push_back( reply( _( "Quem é você?" ), topic( "IDENTITY" ) ) );
        root.responses.push_back( reply( _( "O que está acontecendo lá fora?" ), topic( "SITUATION" ) ) );
        if( trader ) {
            root.responses.push_back( reply( _( "Vamos fazer uma troca." ), topic( "BARTER" ) ) );
        }
        if( quest_giver ) {
            root.responses.push_back( reply( _( "Posso fazer alguma coisa por você?" ),
                                             "TALK_MISSION_LIST" ) );
        }
        if( recruitable ) {
            root.responses.push_back( reply( _( "Que tal sobrevivermos juntos?" ), topic( "RECRUIT" ) ) );
        }
        if( instructor ) {
            root.responses.push_back( reply( _( "Você pode me ensinar alguma coisa?" ), "TALK_TRAIN" ) );
        }
        root.responses.insert( root.responses.end(), modifier_links.begin(), modifier_links.end() );
        root.responses.push_back( reply( _( "Até mais." ), "TALK_DONE" ) );
        include( !hostile, root );
    }
    {
        dialogue_topic_node threat = node( topic( "THREAT" ), foda_kind::alternative,
                                           _( "Atitude = Hostil" ),
                                           string_format( _( "Nem mais um passo!  Este canto do abrigo é do %s.  "
                                                   "Saia agora ou eu te faço sair." ), cfg.name ) );
        threat.responses.push_back( reply( _( "Calma, já estou indo embora." ), "TALK_DONE",
                                           "player_leaving" ) );
        threat.responses.push_back( persuade( _( "Não quero problemas.  Dá pra dividir o espaço." ),
                                              6, topic( "THREAT_CALM" ), "player_leaving",
                                              "TALK_DONE", "hostile" ) );
        threat.responses.push_back( reply( _( "Então vem me tirar daqui." ), "TALK_DONE", "hostile" ) );
        include( hostile, threat );

        dialogue_topic_node calm = node( topic( "THREAT_CALM" ), foda_kind::optional,
                                         _( "Atitude = Hostil" ),
                                         _( "…Tá.  Mas fique longe das minhas coisas e não me dê as costas." ) );
        calm.responses.insert( calm.responses.end(), modifier_links.begin(), modifier_links.end() );
        calm.responses.push_back( reply( _( "Entendido." ), "TALK_DONE" ) );
        include( hostile, calm );
    }

    {
        dialogue_topic_node situation = node( topic( "SITUATION" ), foda_kind::mandatory,
                                              _( "Atitude ≠ Hostil" ),
                                              _( "Mandaram todo mundo para os abrigos de evacuação e prometeram ônibus.  "
                                                      "Ninguém veio.  O rádio só chia, e lá fora os mortos andam pelas ruas." ) );
        situation.responses.push_back( reply( _( "Entendi.  Vamos falar de outra coisa." ), topic( "ROOT" ) ) );
        situation.responses.push_back( reply( _( "Até mais." ), "TALK_DONE" ) );
        include( !hostile, situation );
    }

    {
        dialogue_topic_node identity = node( topic( "IDENTITY" ), foda_kind::mandatory,
                                             _( "Identidade ∧ Atitude ≠ Hostil" ),
                                             archetype_story( cfg.selected_in( feature_group::archetype ) ) );
        identity.responses.push_back( reply( _( "Entendi.  Vamos falar de outra coisa." ), topic( "ROOT" ) ) );
        identity.responses.push_back( reply( _( "Até mais." ), "TALK_DONE" ) );
        include( !hostile, identity );
    }

    {
        dialogue_topic_node barter = node( topic( "BARTER" ), foda_kind::or_group,
                                           _( "Comerciante ∧ Atitude ≠ Hostil" ),
                                           _( "Tenho umas coisas que podem te interessar.  Tudo na base da troca." ) );
        barter.responses.push_back( reply( _( "Mostra o que você tem." ), topic( "ROOT" ), "start_trade" ) );
        barter.responses.push_back( reply( _( "Deixa pra lá." ), topic( "ROOT" ) ) );
        include( trader && !hostile, barter );
    }
    {
        const bool friendly = att == feature::att_friendly;
        std::string line;
        if( friendly ) {
            line = _( "Achei que você nunca ia perguntar!" );
        } else if( att == feature::att_cautious ) {
            line = _( "Te conheço há cinco minutos.  Me dá um bom motivo." );
        } else {
            line = _( "Hmm…  Por que eu deveria confiar em você?" );
        }
        dialogue_topic_node recruit = node( topic( "RECRUIT" ), foda_kind::or_group,
                                            _( "Recrutável ∧ Atitude ≠ Hostil" ), line );
        if( friendly ) {
            recruit.responses.push_back( reply( _( "Então vamos." ), topic( "RECRUIT_ACCEPT" ), "follow" ) );
        } else {
            const int difficulty = att == feature::att_cautious ? 4 : 1;
            recruit.responses.push_back( persuade( _( "Juntos temos mais chance do que sozinhos." ),
                                                   difficulty, topic( "RECRUIT_ACCEPT" ), "follow", topic( "RECRUIT_REFUSE" ) ) );
        }
        recruit.responses.push_back( reply( _( "Esquece." ), topic( "ROOT" ) ) );
        include( recruitable && !hostile, recruit );

        dialogue_topic_node accept = node( topic( "RECRUIT_ACCEPT" ), foda_kind::mandatory,
                                           _( "Recrutável ∧ Atitude ≠ Hostil" ),
                                           _( "Certo, estou com você.  Vamos dar o fora daqui." ) );
        accept.responses.push_back( reply( _( "Vamos." ), "TALK_DONE" ) );
        include( recruitable && !hostile, accept );

        dialogue_topic_node refuse = node( topic( "RECRUIT_REFUSE" ), foda_kind::optional,
                                           _( "Recrutável ∧ Atitude ∈ {Neutro, Cauteloso}" ),
                                           _( "Não.  Ainda não confio em você." ) );
        refuse.responses.push_back( reply( _( "Tudo bem." ), topic( "ROOT" ) ) );
        include( recruitable && !hostile && !friendly, refuse );
    }

    const std::string back_topic = hostile ? topic( "THREAT_CALM" ) : topic( "ROOT" );
    {
        dialogue_topic_node lore_node = node( topic( "LORE" ), foda_kind::optional,
                                              _( "Conhece Lore Local" ),
                                              _( "<city> fica aqui perto.  Antes era um lugar tranquilo; agora as ruas "
                                                      "estão tomadas.  Evite o centro e procure as farmácias da periferia." ) );
        lore_node.responses.push_back( reply( _( "Bom saber." ), back_topic ) );
        include( lore, lore_node );
    }
    {
        dialogue_topic_node cbm_node = node( topic( "CBM" ), foda_kind::optional,
                                             _( "Possui Biônicos (CBMs)" ),
                                             _( "Você reparou, hein?  Fui cobaia de um programa experimental.  Tenho "
                                                     "uma lanterna embutida e placas de blindagem no peito." ) );
        cbm_node.responses.push_back( reply( _( "Impressionante." ), back_topic ) );
        include( cbm, cbm_node );
    }

    model.root_topic = hostile ? topic( "THREAT" ) : topic( "ROOT" );
    return model;
}

static void write_effect_block( JsonOut &jsout, const std::string &target, const std::string &effect )
{
    jsout.start_object();
    jsout.member( "topic", target );
    if( !effect.empty() ) {
        jsout.member( "effect", effect );
    }
    jsout.end_object();
}

static std::string topic_to_json( const dialogue_topic_node &t )
{
    std::ostringstream os;
    JsonOut jsout( os );
    jsout.start_object();
    jsout.member( "id", t.id );
    jsout.member( "dynamic_line", t.dynamic_line );
    jsout.member( "responses" );
    jsout.start_array();
    for( const dialogue_response_node &r : t.responses ) {
        jsout.start_object();
        jsout.member( "text", r.text );
        if( r.persuade_difficulty ) {
            jsout.member( "trial" );
            jsout.start_object();
            jsout.member( "type", "PERSUADE" );
            jsout.member( "difficulty", *r.persuade_difficulty );
            jsout.end_object();
            jsout.member( "success" );
            write_effect_block( jsout, r.topic, r.effect );
            jsout.member( "failure" );
            write_effect_block( jsout, r.failure_topic, r.failure_effect );
        } else {
            jsout.member( "topic", r.topic );
            if( !r.effect.empty() ) {
                jsout.member( "effect", r.effect );
            }
        }
        jsout.end_object();
    }
    jsout.end_array();
    jsout.end_object();
    return os.str();
}

void register_dialogue_tree( const dialogue_feature_model &model )
{
    for( const std::string &id : all_dialogue_topic_ids() ) {
        reset_talk_topic( id );
    }
    for( const dialogue_topic_node &t : model.topics ) {
        try {
            JsonValue jv = json_loader::from_string( topic_to_json( t ) );
            load_talk_topic( jv.get_object(), "npc_spl" );
        } catch( const JsonError &e ) {
            debugmsg( "Failed to register derived talk topic %s: %s", t.id, e.what() );
        }
    }
}

namespace
{
enum menu_action : int {
    act_header = 0
};
} // namespace

void show_derived_dialogue_tree( const npc_feature_config &cfg )
{
    const dialogue_feature_model model = derive_dialogue_tree( cfg );

    uilist view;
    view.title = _( "Árvore 2 — Feature Model de Diálogos (derivado)" );
    view.text = string_format( _( "Raiz: %s\nTópicos incluídos: %d    Podados: %d" ),
                               colorize( model.root_topic, c_light_green ), static_cast<int>( model.topics.size() ),
                               static_cast<int>( model.pruned.size() ) );
    view.desc_enabled = true;

    const auto add_line = [&view]( const std::string & txt, nc_color color, const std::string & desc ) {
        uilist_entry e( act_header, true, 0, txt, desc );
        e.text_color = color;
        view.entries.push_back( e );
    };

    for( const dialogue_topic_node &t : model.topics ) {
        add_line( string_format( "%s %s", foda_symbol( t.kind ), t.id ), c_light_green,
                  string_format( _( "Presença: %s\n\n\"%s\"" ), t.presence_condition, t.dynamic_line ) );
        for( const dialogue_response_node &r : t.responses ) {
            std::string target = r.topic;
            if( r.persuade_difficulty ) {
                target = string_format( _( "PERSUADIR(%d) ? %s : %s" ), *r.persuade_difficulty, r.topic,
                                        r.failure_topic );
            }
            std::string effect;
            if( !r.effect.empty() || !r.failure_effect.empty() ) {
                effect = string_format( " {%s%s%s}", r.effect,
                                        r.effect.empty() || r.failure_effect.empty() ? "" : " / ", r.failure_effect );
            }
            add_line( string_format( "    └─ \"%s\" → %s%s", r.text, target, effect ), c_white,
                      string_format( _( "Resposta do tópico %s" ), t.id ) );
        }
    }
    if( !model.pruned.empty() ) {
        add_line( _( "── Nós podados pela configuração da Árvore 1 ──" ), c_yellow, std::string() );
        for( const pruned_dialogue_node &p : model.pruned ) {
            add_line( string_format( "  ✕ %s", p.id ), c_dark_gray,
                      string_format( _( "Condição de presença falsa: %s" ), p.presence_condition ) );
        }
    }
    view.query();
}

namespace
{
struct diagram_node {
    std::string label;
    foda_kind kind = foda_kind::mandatory;
    foda_kind decomposition = foda_kind::optional;
    bool boxed = false;
    bool disabled = false;
    bool is_root = false;
    nc_color text_color = c_light_gray;
    std::string tag;
    nc_color tag_color = c_dark_gray;
    std::string info;
    std::optional<feature> feat;
    bool is_name = false;
    char hotkey = 0;
    std::vector<diagram_node> children;
    ImVec2 pos = ImVec2( 0, 0 );
    ImVec2 size = ImVec2( 0, 0 );
    float slot_w = 0.0f;
};

struct diagram_metrics {
    float line_h;
    float char_w;
    float pad_x;
    float pad_y;
    float gap;
    float subtree_gap;
    float level;
    float dot_r;
    float arc_r;
    float thickness;
    bool text_mode;
};

diagram_metrics current_metrics()
{
    diagram_metrics m{};
    m.line_h = ImGui::GetTextLineHeight();
    m.char_w = ImGui::CalcTextSize( "0" ).x;
    m.pad_x = m.char_w * 0.6f;
    m.pad_y = m.line_h * 0.2f;
    m.gap = m.char_w * 2.0f;
    m.subtree_gap = m.char_w * 2.5f;
    m.level = m.line_h * 4.6f;
    const bool text_mode = m.line_h < 4.0f;
    m.text_mode = text_mode;
    m.dot_r = text_mode ? 0.5f : std::max( 3.0f, m.line_h * 0.22f );
    m.arc_r = m.line_h * 1.3f;
    m.thickness = text_mode ? 0.5f : std::max( 1.0f, m.line_h / 14.0f );
    return m;
}

void measure( diagram_node &n, const diagram_metrics &m )
{
    const ImVec2 text = ImGui::CalcTextSize( n.label.c_str() );
    n.size = ImVec2( text.x + 2 * m.pad_x, text.y + 2 * m.pad_y );
    if( !n.children.empty() ) {
        n.tag.clear();
    }
    n.slot_w = n.size.x;
    if( !n.tag.empty() ) {
        n.slot_w = std::max( n.slot_w, ImGui::CalcTextSize( n.tag.c_str() ).x + m.pad_x );
    }
    for( diagram_node &c : n.children ) {
        measure( c, m );
    }
}

void layout( diagram_node &n, int depth, float top, float &cursor_x, const diagram_metrics &m )
{
    n.pos.y = top + depth * m.level;
    if( n.children.empty() ) {
        n.pos.x = cursor_x + ( n.slot_w - n.size.x ) / 2;
        cursor_x += n.slot_w + m.gap;
        return;
    }
    for( std::size_t i = 0; i < n.children.size(); ++i ) {
        if( depth == 0 && i > 0 ) {
            cursor_x += m.subtree_gap;
        }
        layout( n.children[i], depth + 1, top, cursor_x, m );
    }
    const diagram_node &first = n.children.front();
    const diagram_node &last = n.children.back();
    const float center = ( first.pos.x + first.size.x / 2 + last.pos.x + last.size.x / 2 ) / 2;
    n.pos.x = center - n.size.x / 2;
}

void shift( diagram_node &n, float dx )
{
    n.pos.x += dx;
    for( diagram_node &c : n.children ) {
        shift( c, dx );
    }
}

void extent( const diagram_node &n, float &min_x, float &max_x, float &max_y )
{
    min_x = std::min( min_x, n.pos.x );
    max_x = std::max( max_x, n.pos.x + n.size.x );
    max_y = std::max( max_y, n.pos.y + n.size.y + ( n.tag.empty() ? 0.0f : ImGui::CalcTextSize(
                          n.tag.c_str() ).y ) );
    for( const diagram_node &c : n.children ) {
        extent( c, min_x, max_x, max_y );
    }
}

float arrange( diagram_node &root, float left, float top, const diagram_metrics &m, float &width )
{
    measure( root, m );
    float cursor = left;
    layout( root, 0, top, cursor, m );
    float min_x = left;
    float max_x = left;
    float max_y = top;
    extent( root, min_x, max_x, max_y );
    if( min_x < left ) {
        shift( root, left - min_x );
        max_x += left - min_x;
    }
    width = max_x;
    return max_y;
}

ImVec2 add( const ImVec2 &a, const ImVec2 &b )
{
    return ImVec2( a.x + b.x, a.y + b.y );
}

ImVec2 top_center( const diagram_node &n, const ImVec2 &o )
{
    return ImVec2( o.x + n.pos.x + n.size.x / 2, o.y + n.pos.y );
}

ImVec2 bottom_center( const diagram_node &n, const ImVec2 &o )
{
    return ImVec2( o.x + n.pos.x + n.size.x / 2, o.y + n.pos.y + n.size.y );
}

void draw_centered_text( ImDrawList *dl, const ImVec2 &center_top, const std::string &text,
                         ImU32 col )
{
    float y = center_top.y;
    const float line_h = ImGui::GetTextLineHeight();
    std::size_t start = 0;
    while( start <= text.size() ) {
        std::size_t end = text.find( '\n', start );
        if( end == std::string::npos ) {
            end = text.size();
        }
        const std::string line = text.substr( start, end - start );
        const float w = ImGui::CalcTextSize( line.c_str() ).x;
        dl->AddText( ImVec2( center_top.x - w / 2, y ), col, line.c_str() );
        y += line_h;
        start = end + 1;
    }
}

void draw_edges( ImDrawList *dl, const diagram_node &n, const ImVec2 &o, const diagram_metrics &m,
                 std::vector<std::pair<ImVec2, bool>> &dots )
{
    if( n.children.empty() ) {
        return;
    }
    const ImU32 col_edge = cataimgui::ImU32_from_color( c_light_gray );
    const ImU32 col_dim = cataimgui::ImU32_from_color( c_dark_gray );
    const ImVec2 p = bottom_center( n, o );
    for( const diagram_node &c : n.children ) {
        dl->AddLine( p, top_center( c, o ), c.disabled ? col_dim : col_edge, m.thickness );
    }
    if( n.decomposition == foda_kind::alternative || n.decomposition == foda_kind::or_group ) {
        const ImVec2 qf = top_center( n.children.front(), o );
        const ImVec2 ql = top_center( n.children.back(), o );
        const float a_first = std::atan2( qf.y - p.y, qf.x - p.x );
        const float a_last = std::atan2( ql.y - p.y, ql.x - p.x );
        if( n.decomposition == foda_kind::alternative ) {
            dl->PathArcTo( p, m.arc_r, a_last, a_first );
            dl->PathStroke( col_edge, 0, m.thickness * 1.4f );
        } else {
            dl->PathLineTo( p );
            dl->PathArcTo( p, m.arc_r, a_last, a_first );
            dl->PathFillConvex( col_edge );
        }
    } else {
        for( const diagram_node &c : n.children ) {
            dots.emplace_back( top_center( c, o ), c.kind == foda_kind::mandatory );
        }
    }
    for( const diagram_node &c : n.children ) {
        draw_edges( dl, c, o, m, dots );
    }
}

struct diagram_events {
    std::optional<feature> clicked_feature;
    bool clicked_name = false;
    std::string hovered_info;
};

void draw_nodes( ImDrawList *dl, const diagram_node &n, const ImVec2 &o, const diagram_metrics &m,
                 diagram_events &ev, std::map<feature, ImVec2> &anchors, int &id )
{
    const ImVec2 a = add( o, n.pos );
    const ImVec2 b = add( a, n.size );
    const ImU32 col_text = cataimgui::ImU32_from_color( n.text_color );
    const bool clickable = ( n.feat && !n.disabled ) || n.is_name;

    ImGui::PushID( id++ );
    ImGui::SetCursorScreenPos( a );
    ImGui::InvisibleButton( "node", n.size );
    const bool hovered = ImGui::IsItemHovered();
    if( clickable && ImGui::IsItemClicked() ) {
        if( n.is_name ) {
            ev.clicked_name = true;
        } else {
            ev.clicked_feature = n.feat;
        }
    }
    ImGui::PopID();
    if( hovered && !n.info.empty() ) {
        ev.hovered_info = n.info;
    }

    if( n.boxed ) {
        dl->AddRect( a, b, cataimgui::ImU32_from_color( n.is_root ? c_white : c_light_green ), 0.0f, 0,
                     m.thickness * 1.5f );
    }
    if( hovered && clickable ) {
        dl->AddRect( ImVec2( a.x - 2, a.y - 2 ), ImVec2( b.x + 2, b.y + 2 ),
                     cataimgui::ImU32_from_color( c_light_cyan ), 0.0f, 0, m.thickness );
    }
    const ImVec2 text_top( ( a.x + b.x ) / 2, a.y + m.pad_y );
    draw_centered_text( dl, text_top, n.label, col_text );
    if( n.is_root ) {
        draw_centered_text( dl, ImVec2( text_top.x + 1, text_top.y ), n.label, col_text );
    }
    if( n.hotkey != 0 ) {
        const std::string key( 1, n.hotkey );
        dl->AddText( ImVec2( a.x - m.char_w * 1.3f, a.y + m.pad_y ),
                     cataimgui::ImU32_from_color( c_light_cyan ), key.c_str() );
    }
    if( !n.tag.empty() ) {
        draw_centered_text( dl, ImVec2( ( a.x + b.x ) / 2, b.y + 1 ), n.tag,
                            cataimgui::ImU32_from_color( n.tag_color ) );
    }
    if( n.feat ) {
        anchors[*n.feat] = ImVec2( ( a.x + b.x ) / 2, b.y );
    }
    for( const diagram_node &c : n.children ) {
        draw_nodes( dl, c, o, m, ev, anchors, id );
    }
}

void draw_dots( ImDrawList *dl, const std::vector<std::pair<ImVec2, bool>> &dots,
                const diagram_metrics &m )
{
    const ImU32 col = cataimgui::ImU32_from_color( c_light_gray );
    const ImU32 bg = ImGui::GetColorU32( ImGuiCol_WindowBg );
    for( const std::pair<ImVec2, bool> &d : dots ) {
        if( d.second ) {
            dl->AddCircleFilled( d.first, m.dot_r, col );
        } else {
            dl->AddCircleFilled( d.first, m.dot_r, bg );
            dl->AddCircle( d.first, m.dot_r, col, 0, m.thickness );
        }
    }
}

void draw_arrow_head( ImDrawList *dl, const ImVec2 &tip, float size, ImU32 col )
{
    dl->AddTriangleFilled( tip, ImVec2( tip.x - size, tip.y + size * 1.7f ),
                           ImVec2( tip.x + size, tip.y + size * 1.7f ), col );
}

void draw_label( ImDrawList *dl, const ImVec2 &center, const std::string &text, ImU32 col )
{
    const ImVec2 sz = ImGui::CalcTextSize( text.c_str() );
    const ImVec2 a( center.x - sz.x / 2 - 3, center.y - sz.y / 2 );
    dl->AddRectFilled( a, ImVec2( a.x + sz.x + 6, a.y + sz.y ), ImGui::GetColorU32( ImGuiCol_WindowBg ) );
    dl->AddText( ImVec2( a.x + 3, a.y ), col, text.c_str() );
}

float constraints_height( const diagram_metrics &m )
{
    int excludes_total = 0;
    for( const cross_tree_constraint &c : all_constraints() ) {
        excludes_total += c.type == constraint_type::excludes_feature ? 1 : 0;
    }
    const float deepest = excludes_total > 0 ? m.line_h * ( 3.4f + 0.9f * ( excludes_total - 1 ) ) :
                          m.line_h * 2.0f;
    return m.line_h * 0.6f + deepest * 0.75f + m.line_h * 1.4f;
}

float draw_constraints( ImDrawList *dl, const npc_feature_config &cfg,
                        const std::map<feature, ImVec2> &anchors, float base_y, const diagram_metrics &m )
{
    const float y0 = base_y + m.line_h * 0.6f;
    float deepest = 0.0f;
    int excludes_seen = 0;
    int excludes_total = 0;
    for( const cross_tree_constraint &c : all_constraints() ) {
        excludes_total += c.type == constraint_type::excludes_feature ? 1 : 0;
    }
    for( const cross_tree_constraint &c : all_constraints() ) {
        const auto ia = anchors.find( c.from );
        const auto ib = anchors.find( c.to );
        if( ia == anchors.end() || ib == anchors.end() ) {
            continue;
        }
        const bool exc = c.type == constraint_type::excludes_feature;
        const bool active = cfg.is_selected( c.from );
        const nc_color color = !active ? c_dark_gray : exc ? c_light_red : c_light_cyan;
        const ImU32 col = cataimgui::ImU32_from_color( color );
        const float d = exc ? m.line_h * ( 3.4f + 0.9f * excludes_seen++ ) : m.line_h * 2.0f;
        deepest = std::max( deepest, d );
        const ImVec2 pa( ia->second.x, y0 );
        const ImVec2 pb( ib->second.x, y0 );
        dl->AddBezierCubic( pa, ImVec2( pa.x, y0 + d ), ImVec2( pb.x, y0 + d ), pb, col, m.thickness );
        const float head = m.line_h * 0.28f;
        draw_arrow_head( dl, pb, head, col );
        if( exc ) {
            draw_arrow_head( dl, pa, head, col );
        }
        const ImVec2 mid( ( pa.x + pb.x ) / 2, y0 + d * 0.75f );
        if( !exc ) {
            draw_label( dl, mid, string_format( _( "%s REQUER %s" ), info( c.from ).label,
                                                info( c.to ).label ), col );
        } else if( excludes_seen == excludes_total ) {
            draw_label( dl, ImVec2( mid.x, mid.y + m.line_h * 0.6f ),
                        string_format( _( "%s EXCLUI todos os %s" ), info( c.from ).label,
                                       info( feature_group::roles ).label ), col );
        }
    }
    return y0 + deepest * 0.75f + m.line_h * 1.4f;
}

std::string diagram_label( feature f )
{
    switch( f ) {
        case feature::role_trader:
            return _( "Comerciante\n(Escambo)" );
        case feature::role_quest_giver:
            return _( "Dador de\nMissões" );
        case feature::role_recruitable:
            return _( "Recrutável\n(Seguidor)" );
        case feature::mod_bionics:
            return _( "Possui\nBiônicos" );
        case feature::mod_local_lore:
            return _( "Conhece\nLore Local" );
        default:
            return info( f ).label;
    }
}

const std::string feature_keys = "1234asdfqwerzx";

diagram_node feature_leaf( const npc_feature_config &cfg, feature f, foda_kind kind )
{
    diagram_node n;
    n.label = diagram_label( f );
    n.kind = kind;
    n.feat = f;
    n.hotkey = feature_keys[static_cast<std::size_t>( f )];
    const bool blocked = cfg.is_blocked( f );
    const bool selected = cfg.is_selected( f );
    n.boxed = selected;
    n.disabled = blocked;
    n.text_color = blocked ? c_dark_gray : selected ? c_light_green : c_yellow;
    const std::string reason = cfg.constraint_reason( f );
    if( blocked ) {
        n.tag = _( "[BLOQUEADO -\nRegra FODA]" );
        n.tag_color = c_dark_gray;
    } else if( cfg.is_forced( f ) ) {
        n.tag = _( "[EXIGIDO -\nRegra FODA]" );
        n.tag_color = c_light_cyan;
    }
    n.info = string_format( "%s: %s", info( f ).label, info( f ).description );
    if( !reason.empty() ) {
        n.info += string_format( _( "  [%s - Regra FODA: %s]" ), blocked ? _( "BLOQUEADO" ) : _( "EXIGIDO" ),
                                 reason );
    }
    return n;
}

diagram_node group_node( const npc_feature_config &cfg, feature_group g, const std::string &label )
{
    const group_info &gi = info( g );
    diagram_node n;
    n.label = label;
    n.kind = gi.presence;
    n.decomposition = gi.decomposition;
    const foda_kind child_kind = gi.decomposition == foda_kind::optional ? foda_kind::optional :
                                 foda_kind::mandatory;
    bool any = false;
    for( const feature_info &fi : all_features() ) {
        if( fi.group == g ) {
            n.children.push_back( feature_leaf( cfg, fi.id, child_kind ) );
            any = any || cfg.is_selected( fi.id );
        }
    }
    n.boxed = gi.presence == foda_kind::mandatory || any;
    n.text_color = n.boxed ? c_light_green : c_yellow;
    switch( gi.decomposition ) {
        case foda_kind::alternative:
            n.info = string_format( _( "%s: grupo alternativo (XOR), exatamente uma característica filha." ),
                                    gi.label );
            break;
        case foda_kind::or_group:
            n.info = string_format( _( "%s: grupo inclusivo (OR), zero ou mais características filhas." ),
                                    gi.label );
            break;
        default:
            n.info = string_format( _( "%s: cada característica filha é opcional." ), gi.label );
            break;
    }
    return n;
}

diagram_node build_tree1( const npc_feature_config &cfg )
{
    diagram_node root;
    root.label = _( "NPC Inicial" );
    root.is_root = true;
    root.text_color = c_white;
    root.info = _( "Raiz da Árvore 1: o Feature Model do NPC, que o jogador configura." );

    diagram_node identity;
    identity.label = _( "Identidade" );
    identity.boxed = true;
    identity.text_color = c_light_green;
    identity.info = _( "Identidade: característica obrigatória, todo NPC tem um nome." );
    diagram_node name;
    name.label = string_format( _( "Nome:\n%s" ), cfg.name );
    name.is_name = true;
    name.boxed = true;
    name.hotkey = 'n';
    name.text_color = c_light_green;
    name.info = _( "Nome: obrigatório. Clique ou aperte n para editar." );
    identity.children.push_back( name );

    root.children.push_back( identity );
    root.children.push_back( group_node( cfg, feature_group::attitude, _( "Atitude" ) ) );
    root.children.push_back( group_node( cfg, feature_group::archetype, _( "Classe /\nArquétipo" ) ) );
    root.children.push_back( group_node( cfg, feature_group::roles, _( "Papéis\nFuncionais" ) ) );
    root.children.push_back( group_node( cfg, feature_group::modifiers, _( "Modificadores" ) ) );
    return root;
}

struct dialogue_box {
    std::string topic_id;
    std::string caption_id;
    std::string caption_origin;
    std::string text;
    std::vector<std::pair<std::string, nc_color>> exits;
    std::string info;
    bool builtin = false;
    bool is_root = false;
    int depth = 0;
    std::vector<std::pair<std::string, std::size_t>> edges;
    ImVec2 pos = ImVec2( 0, 0 );
    ImVec2 size = ImVec2( 0, 0 );
};

std::string wrap_to_width( const std::string &text, float max_w )
{
    std::istringstream words( text );
    std::string word;
    std::string line;
    std::string out;
    while( words >> word ) {
        const std::string candidate = line.empty() ? word : line + " " + word;
        if( !line.empty() && ImGui::CalcTextSize( candidate.c_str() ).x > max_w ) {
            out += line + "\n";
            line = word;
        } else {
            line = candidate;
        }
    }
    return out + line;
}

std::string short_topic( const std::string &id )
{
    return id.rfind( topic_prefix, 0 ) == 0 ? id.substr( topic_prefix.size() ) : id;
}

std::string effect_note( const std::string &effect )
{
    if( effect == "start_trade" ) {
        return _( "abre a tela de troca" );
    } else if( effect == "follow" ) {
        return _( "o NPC passa a te seguir" );
    } else if( effect == "hostile" ) {
        return _( "o NPC ataca você" );
    } else if( effect == "player_leaving" ) {
        return _( "o NPC te dá tempo para sair" );
    }
    return effect;
}

std::string displayed_line( const std::string &line )
{
    std::string out = line;
    const std::string tag = "<city>";
    const std::size_t at = out.find( tag );
    if( at != std::string::npos ) {
        out.replace( at, tag.size(), _( "[cidade mais próxima]" ) );
    }
    return out;
}

struct dialogue_graph {
    std::vector<dialogue_box> boxes;
    std::vector<std::string> pruned;
};

dialogue_graph build_dialogue_graph( const npc_feature_config &cfg, const diagram_metrics &m )
{
    const dialogue_feature_model model = derive_dialogue_tree( cfg );
    const float text_w = m.char_w * 34.0f;
    const float label_w = m.char_w * 24.0f;
    std::map<std::string, const dialogue_topic_node *> by_id;
    for( const dialogue_topic_node &t : model.topics ) {
        by_id[t.id] = &t;
    }
    dialogue_graph g;
    for( const pruned_dialogue_node &p : model.pruned ) {
        g.pruned.push_back( short_topic( p.id ) );
    }

    std::map<std::string, std::size_t> index;
    std::vector<std::size_t> queue;
    const auto add_box = [&]( const std::string & id, int depth ) {
        dialogue_box b;
        b.topic_id = id;
        b.depth = depth;
        b.caption_id = short_topic( id );
        const auto it = by_id.find( id );
        if( it != by_id.end() ) {
            b.caption_origin = it->second->presence_condition;
            b.text = wrap_to_width( "\"" + displayed_line( it->second->dynamic_line ) + "\"", text_w );
        } else if( id == "TALK_MISSION_LIST" ) {
            b.builtin = true;
            b.caption_origin = _( "Dador de Missões ∧ Atitude ≠ Hostil" );
            b.text = wrap_to_width( _( "(menu do jogo) O NPC mostra a missão que tem para você e você pode aceitá-la." ),
                                    text_w );
        } else {
            b.builtin = true;
            b.caption_origin = _( "Instrutor ∧ Atitude ≠ Hostil" );
            b.text = wrap_to_width( _( "(menu do jogo) O NPC lista as perícias que pode te ensinar." ), text_w );
        }
        b.info = string_format( _( "%s  [presença: %s]" ), id, b.caption_origin );
        g.boxes.push_back( b );
        index[id] = g.boxes.size() - 1;
        queue.push_back( g.boxes.size() - 1 );
        return g.boxes.size() - 1;
    };
    const auto known = [&by_id]( const std::string & id ) {
        return by_id.count( id ) > 0 || id == "TALK_MISSION_LIST" || id == "TALK_TRAIN";
    };

    g.boxes.reserve( 16 );
    add_box( model.root_topic, 0 );
    g.boxes.front().is_root = true;
    for( std::size_t q = 0; q < queue.size(); ++q ) {
        const std::size_t i = queue[q];
        const auto it = by_id.find( g.boxes[i].topic_id );
        if( it == by_id.end() ) {
            continue;
        }
        for( const dialogue_response_node &r : it->second->responses ) {
            struct outcome {
                std::string target;
                std::string suffix;
                std::string effect;
            };
            std::vector<outcome> outcomes;
            if( r.persuade_difficulty ) {
                outcomes.push_back( { r.topic, string_format( _( " [persuasão %d: sucesso]" ), *r.persuade_difficulty ), r.effect } );
                outcomes.push_back( { r.failure_topic, _( " [persuasão: falha]" ), r.failure_effect } );
            } else {
                outcomes.push_back( { r.topic, std::string(), r.effect } );
            }
            for( const outcome &o : outcomes ) {
                std::string label = "\"" + r.text + "\"" + o.suffix;
                const std::string note = o.effect.empty() ? std::string() : " {" + effect_note( o.effect ) + "}";
                if( o.target == "TALK_DONE" ) {
                    g.boxes[i].exits.emplace_back( wrap_to_width( "■ " + label + note + _( " → fim da conversa" ), text_w ),
                                                   c_light_gray );
                } else if( index.count( o.target ) > 0 ) {
                    g.boxes[i].exits.emplace_back( wrap_to_width( "↩ " + label + note + " → " + short_topic( o.target ),
                                                   text_w ), c_light_cyan );
                } else if( known( o.target ) ) {
                    const std::size_t child = add_box( o.target, g.boxes[i].depth + 1 );
                    g.boxes[i].edges.emplace_back( wrap_to_width( label + note, label_w ), child );
                }
            }
        }
    }
    for( dialogue_box &b : g.boxes ) {
        ImVec2 sz = ImGui::CalcTextSize( b.text.c_str() );
        for( const std::pair<std::string, nc_color> &e : b.exits ) {
            const ImVec2 es = ImGui::CalcTextSize( e.first.c_str() );
            sz.x = std::max( sz.x, es.x );
            sz.y += es.y;
        }
        b.size = ImVec2( sz.x + 2 * m.pad_x, sz.y + 2 * m.pad_y + ( b.exits.empty() ? 0.0f : m.pad_y ) );
    }
    return g;
}

float place_dialogue( dialogue_graph &g, std::size_t i, float &cursor_y, const std::vector<float> &col_x,
                      const diagram_metrics &m )
{
    dialogue_box &b = g.boxes[i];
    const float caption_h = m.line_h * 1.1f;
    const float vgap = m.line_h * 0.9f;
    b.pos.x = col_x[b.depth];
    const float start = cursor_y;
    if( b.edges.empty() ) {
        b.pos.y = cursor_y + caption_h;
    } else {
        float first = 0.0f;
        float last = 0.0f;
        for( std::size_t k = 0; k < b.edges.size(); ++k ) {
            place_dialogue( g, b.edges[k].second, cursor_y, col_x, m );
            const dialogue_box &c = g.boxes[b.edges[k].second];
            const float center = c.pos.y + c.size.y / 2;
            if( k == 0 ) {
                first = center;
            }
            last = center;
        }
        g.boxes[i].pos.y = std::max( ( first + last ) / 2 - g.boxes[i].size.y / 2, start + caption_h );
    }
    const dialogue_box &placed = g.boxes[i];
    cursor_y = std::max( cursor_y, placed.pos.y + placed.size.y + vgap );
    return cursor_y;
}

ImVec2 draw_dialogue_graph( ImDrawList *dl, const npc_feature_config &cfg, const ImVec2 &o,
                            const diagram_metrics &m, diagram_events &ev, std::vector<std::string> &pruned )
{
    dialogue_graph g = build_dialogue_graph( cfg, m );
    pruned = g.pruned;

    int max_depth = 0;
    for( const dialogue_box &b : g.boxes ) {
        max_depth = std::max( max_depth, b.depth );
    }
    std::vector<float> col_w( max_depth + 1, 0.0f );
    for( const dialogue_box &b : g.boxes ) {
        col_w[b.depth] = std::max( col_w[b.depth], b.size.x );
    }
    const float label_space = m.char_w * 36.0f;
    std::vector<float> col_x( max_depth + 1, m.char_w * 1.5f );
    for( int d = 1; d <= max_depth; ++d ) {
        col_x[d] = col_x[d - 1] + col_w[d - 1] + label_space;
    }
    float cursor_y = 0.0f;
    const float bottom = place_dialogue( g, 0, cursor_y, col_x, m );

    const ImU32 col_edge = cataimgui::ImU32_from_color( c_light_gray );
    const ImU32 col_label_bg = cataimgui::ImU32_from_color( c_white );
    const ImU32 col_label_fg = cataimgui::ImU32_from_color( c_black );
    const ImU32 col_box_bg = ImGui::GetColorU32( ImVec4( 0.10f, 0.10f, 0.12f, 1.0f ) );

    for( const dialogue_box &b : g.boxes ) {
        for( const std::pair<std::string, std::size_t> &e : b.edges ) {
            const dialogue_box &c = g.boxes[e.second];
            const ImVec2 p0( o.x + b.pos.x + b.size.x, o.y + b.pos.y + b.size.y / 2 );
            const ImVec2 p3( o.x + c.pos.x - m.char_w * 0.4f, o.y + c.pos.y + c.size.y / 2 );
            const float head = m.line_h * 0.3f;
            const ImVec2 ls = ImGui::CalcTextSize( e.first.c_str() );
            const float label_right = p3.x - head * 2.0f;
            const float label_left = label_right - ls.x - m.pad_x * 1.2f;
            const ImVec2 bend_end( label_left - m.char_w, p3.y );
            const float dx = ( bend_end.x - p0.x ) * 0.5f;
            dl->AddBezierCubic( p0, ImVec2( p0.x + dx, p0.y ), ImVec2( bend_end.x - dx, bend_end.y ), bend_end,
                                col_edge, m.thickness );
            dl->AddLine( bend_end, p3, col_edge, m.thickness );
            dl->AddTriangleFilled( p3, ImVec2( p3.x - head * 1.7f, p3.y - head ),
                                   ImVec2( p3.x - head * 1.7f, p3.y + head ), col_edge );
            const ImVec2 a( label_left, p3.y - ls.y / 2 - m.pad_y * 0.5f );
            const ImVec2 z( label_right, p3.y + ls.y / 2 + m.pad_y * 0.5f );
            dl->AddRectFilled( a, z, col_label_bg );
            dl->AddText( ImVec2( a.x + m.pad_x * 0.6f, a.y + m.pad_y * 0.5f ), col_label_fg, e.first.c_str() );
        }
    }

    int id = 1000;
    float right = 0.0f;
    for( const dialogue_box &b : g.boxes ) {
        const ImVec2 a( o.x + b.pos.x, o.y + b.pos.y );
        const ImVec2 z( a.x + b.size.x, a.y + b.size.y );
        right = std::max( right, b.pos.x + b.size.x );
        ImGui::PushID( id++ );
        ImGui::SetCursorScreenPos( a );
        ImGui::InvisibleButton( "topic", b.size );
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        if( hovered ) {
            ev.hovered_info = b.info;
        }
        dl->AddRectFilled( a, z, col_box_bg );
        const nc_color border = b.is_root ? c_white : b.builtin ? c_light_cyan : c_light_green;
        dl->AddRect( a, z, cataimgui::ImU32_from_color( hovered ? c_yellow : border ), 0.0f, 0,
                     m.thickness * ( b.is_root ? 2.0f : 1.4f ) );
        const std::string cap_id = b.caption_id + "  ";
        const ImVec2 cap( a.x, a.y - m.line_h * 1.05f );
        dl->AddText( cap, cataimgui::ImU32_from_color( c_light_cyan ), cap_id.c_str() );
        const std::string origin = "← " + b.caption_origin;
        dl->AddText( ImVec2( cap.x + ImGui::CalcTextSize( cap_id.c_str() ).x, cap.y ),
                     cataimgui::ImU32_from_color( c_yellow ), origin.c_str() );
        float y = a.y + m.pad_y;
        dl->AddText( ImVec2( a.x + m.pad_x, y ), cataimgui::ImU32_from_color( b.builtin ? c_light_cyan : c_white ),
                     b.text.c_str() );
        y += ImGui::CalcTextSize( b.text.c_str() ).y + ( b.exits.empty() ? 0.0f : m.pad_y );
        for( const std::pair<std::string, nc_color> &e : b.exits ) {
            dl->AddText( ImVec2( a.x + m.pad_x, y ), cataimgui::ImU32_from_color( e.second ), e.first.c_str() );
            y += ImGui::CalcTextSize( e.first.c_str() ).y;
        }
    }
    return ImVec2( right + m.char_w * 2.0f, bottom );
}

class foda_diagram_window : public cataimgui::window
{
    public:
        explicit foda_diagram_window( npc_feature_config &config ) :
            cataimgui::window( _( "Linha de Produtos de NPC (FODA) — Primeiro NPC do abrigo" ),
                               ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                               ImGuiWindowFlags_NoNav ),
            cfg( config ) {}

        diagram_events events;
        bool clicked_confirm = false;
        bool clicked_cancel = false;
        int active_tab = 0;
        int request_tab = -1;

    protected:
        cataimgui::bounds get_bounds() override {
            return { 0, 0, 1.0, 1.0 };
        }
        void draw_controls() override;

    private:
        npc_feature_config &cfg;
        std::string last_hovered;
        std::vector<std::string> pruned;
};

void foda_diagram_window::draw_controls()
{
    const diagram_metrics m = current_metrics();

    if( ImGui::BeginTabBar( "spl_trees" ) ) {
        const char *tab_names[2] = {
            _( "Árvore 1 — Características do NPC" ),
            _( "Árvore 2 — Árvore de diálogo montada" )
        };
        for( int t = 0; t < 2; ++t ) {
            const ImGuiTabItemFlags flags = request_tab == t ? ImGuiTabItemFlags_SetSelected : 0;
            if( ImGui::BeginTabItem( tab_names[t], nullptr, flags ) ) {
                if( request_tab == -1 || request_tab == t ) {
                    if( active_tab != t ) {
                        last_hovered.clear();
                    }
                    active_tab = t;
                }
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
    request_tab = -1;

    const ImGuiStyle &style = ImGui::GetStyle();
    const float row = m.line_h + style.ItemSpacing.y;
    const int extra_rows = active_tab == 0 ?
                           static_cast<int>( cfg.last_propagation_log().size() ) + ( cfg.is_valid() ? 0 : 1 ) : 2;
    const float footer_h = row * static_cast<float>( 6 + extra_rows ) + ImGui::GetFrameHeightWithSpacing() +
                           style.WindowPadding.y;

    ImGui::BeginChild( "spl_drawing", ImVec2( 0, -footer_h ), ImGuiChildFlags_Borders,
                       ImGuiWindowFlags_HorizontalScrollbar );
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    diagram_events ev;
    ImVec2 content( 0, 0 );
    if( active_tab == 0 ) {
        dl->AddText( origin, cataimgui::ImU32_from_color( c_light_cyan ),
                     _( "Árvore 1 — Feature Model do NPC (o jogador escolhe; a Árvore 2 é montada a partir dela)" ) );
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const float header_h = m.line_h * 1.6f;
        const float base_size = ImGui::GetFont()->LegacySize;
        const float min_scale = 0.6f;
        float scale = 1.0f;
        bool font_pushed = false;
        diagram_metrics tm = m;
        diagram_node tree1;
        float width1 = 0.0f;
        float bottom1 = 0.0f;
        for( int attempt = 0; attempt < 4; ++attempt ) {
            tree1 = build_tree1( cfg );
            bottom1 = arrange( tree1, tm.char_w * 2.0f, 0.0f, tm, width1 );
            const float limit = avail.x - tm.char_w;
            if( tm.text_mode || width1 <= limit || scale <= min_scale ) {
                break;
            }
            scale = std::max( min_scale, scale * limit / width1 * 0.98f );
            if( font_pushed ) {
                ImGui::PopFont();
            }
            ImGui::PushFont( nullptr, base_size * scale );
            font_pushed = true;
            tm = current_metrics();
        }
        const float height1 = bottom1 + constraints_height( tm );
        const float offset_x = std::max( 0.0f, ( avail.x - width1 ) / 2 );
        const float offset_y = std::max( 0.0f, ( avail.y - header_h - height1 ) / 2 );
        const ImVec2 tree_origin( origin.x + offset_x, origin.y + header_h + offset_y );
        std::vector<std::pair<ImVec2, bool>> dots;
        std::map<feature, ImVec2> anchors;
        int id = 0;
        draw_edges( dl, tree1, tree_origin, tm, dots );
        draw_nodes( dl, tree1, tree_origin, tm, ev, anchors, id );
        draw_dots( dl, dots, tm );
        const float bottom = draw_constraints( dl, cfg, anchors, tree_origin.y + bottom1, tm ) - origin.y;
        if( font_pushed ) {
            ImGui::PopFont();
        }
        content = ImVec2( offset_x + width1, bottom );
    } else {
        dl->AddText( origin, cataimgui::ImU32_from_color( c_light_cyan ),
                     _( "Árvore 2 — Diálogo do NPC montado a partir das características escolhidas na Árvore 1" ) );
        const ImVec2 graph_origin( origin.x, origin.y + m.line_h * 2.0f );
        const ImVec2 size = draw_dialogue_graph( dl, cfg, graph_origin, m, ev, pruned );
        content = ImVec2( size.x, size.y + m.line_h * 2.0f );
    }
    ImGui::SetCursorScreenPos( ImVec2( origin.x, origin.y + content.y ) );
    ImGui::Dummy( ImVec2( content.x, m.line_h ) );
    ImGui::EndChild();

    if( ev.clicked_feature || ev.clicked_name ) {
        events.clicked_feature = ev.clicked_feature;
        events.clicked_name = ev.clicked_name;
    }
    if( !ev.hovered_info.empty() ) {
        last_hovered = ev.hovered_info;
    }

    if( active_tab == 0 ) {
        cataimgui::draw_colored_text( _( "Legenda: <color_light_gray>●</color> obrigatória   "
                                         "<color_light_gray>○</color> opcional   arco vazado = alternativa (XOR)   "
                                         "arco cheio = inclusiva (OR)   <color_light_green>caixa</color> = selecionada   "
                                         "<color_dark_gray>cinza</color> = bloqueada   "
                                         "<color_light_cyan>→ REQUER</color>   <color_light_red>↔ EXCLUI</color>" ), c_light_gray,
                                         ImGui::GetContentRegionAvail().x );
    } else {
        cataimgui::draw_colored_text( _( "Legenda: <color_light_green>caixa</color> = fala do NPC   "
                                         "<color_white>etiqueta branca</color> = resposta do jogador   "
                                         "<color_light_cyan>↩</color> volta para outro tópico   ■ encerra a conversa   "
                                         "<color_yellow>← condição</color> = de onde veio na Árvore 1   "
                                         "<color_light_cyan>caixa azul</color> = menu nativo do jogo" ), c_light_gray,
                                         ImGui::GetContentRegionAvail().x );
    }
    cataimgui::draw_colored_text( _( "Teclas: <color_light_cyan>1-4</color> atitude  "
                                     "<color_light_cyan>a s d f</color> classe  <color_light_cyan>q w e r</color> papéis  "
                                     "<color_light_cyan>z x</color> modificadores  <color_light_cyan>n</color> nome  "
                                     "<color_light_cyan>Tab</color> troca de árvore  <color_light_cyan>C</color> confirmar  "
                                     "<color_light_cyan>Esc</color> cancelar" ), c_light_gray, ImGui::GetContentRegionAvail().x );
    cataimgui::draw_colored_text( last_hovered.empty() ?
                                  _( "Passe o mouse sobre um nó para ver os detalhes." ) : last_hovered, c_white,
                                  ImGui::GetContentRegionAvail().x );
    if( active_tab == 0 ) {
        for( const std::string &msg : cfg.last_propagation_log() ) {
            cataimgui::draw_colored_text( string_format( _( "Regra aplicada: %s" ), msg ), c_light_red );
        }
    } else {
        std::string list;
        for( const std::string &p : pruned ) {
            list += ( list.empty() ? "" : ", " ) + p;
        }
        cataimgui::draw_colored_text( string_format( _( "Tópicos podados pela Árvore 1 (não entram nesta conversa): %s" ),
                                      list.empty() ? _( "nenhum" ) : list ), c_dark_gray, ImGui::GetContentRegionAvail().x );
    }
    std::string why;
    const bool valid = cfg.is_valid( &why );
    if( !valid ) {
        cataimgui::draw_colored_text( string_format( _( "Configuração inválida: %s" ), why ), c_light_red );
    }
    ImGui::BeginDisabled( !valid );
    if( ImGui::Button( _( "Confirmar e Iniciar Jogo com este NPC [C]" ) ) ) {
        clicked_confirm = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if( ImGui::Button( active_tab == 0 ? _( "Visualizar Árvore de Diálogos Derivada [Tab]" ) :
                       _( "Voltar para as características [Tab]" ) ) ) {
        request_tab = 1 - active_tab;
    }
    ImGui::SameLine();
    if( ImGui::Button( _( "Cancelar (NPC aleatório padrão) [Esc]" ) ) ) {
        clicked_cancel = true;
    }
}

} // namespace

bool run_configuration_menu( npc_feature_config &cfg )
{
    cfg.validate_and_propagate_constraints();

    input_context ctxt;
    ctxt.register_action( "MOUSE_MOVE" );
    ctxt.register_action( "SELECT" );
    ctxt.register_action( "CONFIRM" );
    ctxt.register_action( "QUIT" );
    ctxt.register_action( "ANY_INPUT" );
    ctxt.set_timeout( 10 );

    foda_diagram_window win( cfg );
    const auto edit_name = [&cfg]() {
        string_input_popup popup;
        popup.title( _( "Nome do NPC:" ) ).width( 30 ).max_length( 30 ).text( cfg.name );
        popup.query();
        if( !popup.canceled() ) {
            cfg.name = popup.text();
            cfg.validate_and_propagate_constraints();
        }
    };

    const std::chrono::steady_clock::time_point accept_input_after =
        std::chrono::steady_clock::now() + std::chrono::milliseconds( 500 );
    while( true ) {
        ui_manager::redraw_invalidated();

        if( win.clicked_cancel ) {
            return false;
        }
        if( win.clicked_confirm && cfg.is_valid() ) {
            return true;
        }
        win.clicked_confirm = false;
        if( win.events.clicked_name ) {
            win.events.clicked_name = false;
            edit_name();
        }
        if( win.events.clicked_feature ) {
            cfg.choose( *win.events.clicked_feature );
            win.events.clicked_feature.reset();
        }

        const std::string action = ctxt.handle_input();
        const int key = ctxt.get_raw_input().get_first_input();
        if( std::chrono::steady_clock::now() < accept_input_after ) {
            continue;
        }
        const std::size_t feature_index = key > 0 && key < 128 ?
                                          feature_keys.find( static_cast<char>( key ) ) : std::string::npos;
        if( feature_index != std::string::npos ) {
            cfg.choose( static_cast<feature>( feature_index ) );
        } else if( key == 'n' ) {
            edit_name();
        } else if( key == '\t' || key == 'V' ) {
            win.request_tab = key == 'V' ? 1 : 1 - win.active_tab;
        } else if( key == 'C' || action == "CONFIRM" ) {
            if( cfg.is_valid() ) {
                return true;
            }
        } else if( key == KEY_ESCAPE || key == 'Q' ) {
            return false;
        }
    }
}

npc_class_id archetype_class( const npc_feature_config &cfg )
{
    switch( cfg.selected_in( feature_group::archetype ) ) {
        case feature::arch_merchant:
            return NC_JUNK_SHOPKEEP;
        case feature::arch_medic:
            return NC_DOCTOR;
        case feature::arch_raider:
            return NC_SCAVENGER;
        case feature::arch_survivor:
        default:
            return NC_NONE;
    }
}

void apply_to_npc( npc &guy, const npc_feature_config &cfg )
{
    guy.name = cfg.name;

    switch( cfg.selected_in( feature_group::attitude ) ) {
        case feature::att_friendly:
            guy.op_of_u.trust += 5;
            guy.op_of_u.value += 3;
            guy.set_attitude( NPCATT_NULL );
            break;
        case feature::att_cautious:
            guy.op_of_u.trust -= 3;
            guy.op_of_u.fear += 2;
            guy.set_attitude( NPCATT_NULL );
            break;
        case feature::att_hostile:
            guy.op_of_u.trust -= 5;
            guy.op_of_u.anger += 5;
            guy.set_attitude( NPCATT_WAIT_FOR_LEAVE );
            guy.patience = 60;
            break;
        case feature::att_neutral:
        default:
            guy.set_attitude( NPCATT_NULL );
            break;
    }

    if( cfg.is_selected( feature::mod_bionics ) ) {
        guy.add_bionic( bio_power_storage );
        guy.add_bionic( bio_flashlight );
        guy.add_bionic( bio_armor_torso );
        guy.set_power_level( guy.get_max_power_level() );
    }

    if( cfg.is_selected( feature::role_instructor ) ) {
        for( const skill_id &sk : {
                 skill_firstaid, skill_melee, skill_survival
             } ) {
            guy.set_skill_level( sk, std::max( 4, static_cast<int>( guy.get_skill_level( sk ) ) ) );
        }
    }

    if( cfg.is_selected( feature::role_quest_giver ) ) {
        std::vector<mission_type_id> candidates;
        for( const mission_type &t : mission_type::get_all() ) {
            if( !t.urgent &&
                std::find( t.origins.begin(), t.origins.end(), ORIGIN_OPENER_NPC ) != t.origins.end() &&
                t.place( guy.pos_abs_omt() ) ) {
                candidates.push_back( t.id );
            }
        }
        const mission_type_id type = random_entry( candidates, mission_type_id::NULL_ID() );
        if( !type.is_null() ) {
            guy.add_new_mission( mission::reserve_new( type, guy.getID() ) );
        }
    }

    const dialogue_feature_model model = derive_dialogue_tree( cfg );
    register_dialogue_tree( model );
    guy.chatbin.first_topic = model.root_topic;

    guy.set_value( spl_value_key, cfg.to_json_string() );
}

void restore_npc_dialogue( const npc &guy )
{
    const diag_value *stored = guy.maybe_get_value( spl_value_key );
    if( stored == nullptr || !stored->is_str() ) {
        return;
    }
    if( std::optional<npc_feature_config> cfg = npc_feature_config::from_json_string( stored->str() ) ) {
        register_dialogue_tree( derive_dialogue_tree( *cfg ) );
    }
}

static std::optional<npc_feature_config> &pending()
{
    static std::optional<npc_feature_config> cfg;
    return cfg;
}

void set_pending_config( const npc_feature_config &cfg )
{
    pending() = cfg;
}

std::optional<npc_feature_config> take_pending_config()
{
    std::optional<npc_feature_config> result = std::move( pending() );
    pending().reset();
    return result;
}

void clear_pending_config()
{
    pending().reset();
}

} // namespace npc_spl
