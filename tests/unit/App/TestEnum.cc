#include "TestEnum.hh"
#include "casm/casm_io/Log.hh"
#include "casm/clex/Supercell.hh"
#include "casm/clex/PrimClex.hh"
#include "casm/clex/ConfigIterator.hh"
#include "casm/clex/ScelEnum.hh"
#include "casm/clex/FilteredConfigIterator.hh"
#include "casm/app/casm_functions.hh"
#include "casm/completer/Handlers.hh"

extern "C" {
  CASM::EnumInterfaceBase *make_TestEnum_interface() {
    return new CASM::EnumInterface<CASM::TestEnum>();
  }
}

namespace CASM {

  const std::string CASM_TMP::traits<TestEnum>::name = "TestEnum";

  const std::string CASM_TMP::traits<TestEnum>::help =
    "TestEnum: \n\n"

    "  supercells: ScelEnum JSON settings (default='{\"existing_only\"=true}')\n"
    "    Indicate supercells to enumerate all occupational configurations in. May \n"
    "    be a JSON array of supercell names, or a JSON object specifying          \n"
    "    supercells in terms of size and unit cell. By default, all existing      \n"
    "    supercells are used. See 'ScelEnum' description for details.         \n\n"

    "  filter: string (optional, default=None)\n"
    "    A query command to use to filter which Configurations are kept.          \n"
    "\n"
    "  Examples:\n"
    "    To enumerate all occupations in supercells up to and including size 4:\n"
    "      casm enum --method TestEnum -i '{\"supercells\": {\"max\": 4}}' \n"
    "\n"
    "    To enumerate all occupations in all existing supercells:\n"
    "      casm enum --method TestEnum\n"
    "\n"
    "    To enumerate all occupations in all particular supercells:\n"
    "      casm enum --method TestEnum -i \n"
    "      '{ \n"
    "        \"supercells\": { \n"
    "          \"name\": [\n"
    "            \"SCEL1_1_1_1_0_0_0\",\n"
    "            \"SCEL2_1_2_1_0_0_0\",\n"
    "            \"SCEL4_1_4_1_0_0_0\"\n"
    "          ]\n"
    "        } \n"
    "      }' \n\n";


  int EnumInterface<TestEnum>::run(
    PrimClex &primclex,
    const jsonParser &_kwargs,
    const Completer::EnumOption &enum_opt) const {

    jsonParser kwargs {_kwargs};
    if(kwargs.is_null()) {
      kwargs = jsonParser::object();
    }

    // default is use all existing Supercells
    jsonParser scel_input;
    scel_input["existing_only"] = true;
    kwargs.get_if(scel_input, "supercells");

    // check supercell shortcuts
    if(enum_opt.vm().count("min")) {
      scel_input["min"] = enum_opt.min_volume();
    }

    if(enum_opt.vm().count("max")) {
      scel_input["max"] = enum_opt.max_volume();
    }

    if(enum_opt.all_existing()) {
      scel_input.erase("min");
      scel_input.erase("max");
      scel_input["existing_only"] = true;
    }

    if(enum_opt.vm().count("scelnames")) {
      scel_input["name"] = enum_opt.supercell_strs();
    }

    ScelEnum scel_enum(primclex, scel_input);

    Log &log = primclex.log();

    Index Ninit = std::distance(primclex.config_begin(), primclex.config_end());
    log << "# configurations in this project: " << Ninit << "\n" << std::endl;

    log.begin(name());

    std::vector<std::string> filter_expr;

    // check shortcuts
    if(enum_opt.vm().count("filter")) {
      filter_expr = enum_opt.filter_strs();
    }
    else if(kwargs.contains("filter")) {
      filter_expr.push_back(kwargs["filter"].get<std::string>());
    };

    for(auto &scel : scel_enum) {

      log << "Enumerate configurations for " << scel.get_name() << " ...  " << std::flush;

      TestEnum enumerator(scel);
      Index num_before = scel.get_config_list().size();
      if(kwargs.contains("filter")) {
        try {
          scel.add_unique_canon_configs(
            filter_begin(
              enumerator.begin(),
              enumerator.end(),
              filter_expr,
              primclex.settings().config_io()),
            filter_end(enumerator.end())
          );
        }
        catch(std::exception &e) {
          primclex.err_log() << "Cannot filter configurations using the expression provided: \n" << e.what() << "\nExiting...\n";
          return ERR_INVALID_ARG;
        }
      }
      else {
        scel.add_unique_canon_configs(enumerator.begin(), enumerator.end());
      }

      log << (scel.get_config_list().size() - num_before) << " configs." << std::endl;
    }
    log << "  DONE." << std::endl << std::endl;

    Index Nfinal = std::distance(primclex.config_begin(), primclex.config_end());

    log << "# new configurations: " << Nfinal - Ninit << "\n";
    log << "# configurations in this project: " << Nfinal << "\n" << std::endl;

    log << "Write SCEL..." << std::endl;
    primclex.print_supercells();
    log << "  DONE" << std::endl << std::endl;

    log << "Writing config_list..." << std::endl;
    primclex.write_config_list();
    log << "  DONE" << std::endl;
    return 0;
  }


  /// \brief Construct with a Supercell, using all permutations
  TestEnum::TestEnum(Supercell &_scel) :
    m_counter(
      Array<int>(_scel.num_sites(), 0),
      _scel.max_allowed_occupation(),
      Array<int>(_scel.num_sites(), 1)) {

    m_current = notstd::make_cloneable<Configuration>(_scel, this->source(0), m_counter());
    reset_properties(*m_current);
    this->_initialize(&(*m_current));

    // Make sure that current() is a primitive canonical config
    if(!_check_current()) {
      increment();
    }

    // set step to 0
    if(valid()) {
      _set_step(0);
    }
    _current().set_source(this->source(step()));
  }

  /// Implements _increment over all occupations
  void TestEnum::increment() {

    bool is_valid_config {false};

    while(!is_valid_config && ++m_counter) {

      _current().set_occupation(m_counter());
      is_valid_config = _check_current();
    }

    if(m_counter.valid()) {
      this->_increment_step();
    }
    else {
      this->_invalidate();
    }
    _current().set_source(this->source(step()));
  }

  /// Returns true if current() is primitive and canonical
  bool TestEnum::_check_current() const {
    return current().is_primitive() && current().is_canonical();
  }

}
