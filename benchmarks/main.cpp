#include "core/Catalog.hpp"
#include "core/Compare.hpp"
#include "core/Document.hpp"
#include "core/Host.hpp"
#include "core/Options.hpp"
#include "core/Policy.hpp"
#include "core/Reporters.hpp"
#include "core/Result.hpp"
#include "core/RunEvents.hpp"
#include "core/Runner.hpp"
#include "core/Sampler.hpp"
#include "core/Workload.hpp"
#include "slicer/Environment.hpp"

#include <boost/nowide/args.hpp>

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace Slic3r::Bench;

namespace {

// Prints the comparison of the two result documents the options name, and returns 1 when a workload
// failed only in b and 3 when a workload's output changed.
int compare_files(const Options& options)
{
    const Result     a          = read_document_file(options.compare->a);
    const Result     b          = read_document_file(options.compare->b);
    const Comparison comparison = compare(a, b, {options.allow_mismatch, options.verbose});
    CompareView      view;
    view.label_a        = options.compare->a;
    view.label_b        = options.compare->b;
    view.collapse_below = options.collapse_below.value_or(view.collapse_below);
    view.sort_by        = options.sort_by.value_or(view.sort_by);
    view.color          = use_color(options.color, enable_terminal_escapes(), std::getenv("NO_COLOR"), std::getenv("TERM"));
    write_comparison(std::cout, comparison, view);
    if (comparison.failed_only_in_b > 0)
        return 1;
    return comparison.changed_outputs > 0 ? 3 : 0;
}

std::vector<CatalogEntry> selected(const Options& options)
{
    return select_entries(read_catalog_dir(ORCABENCH_CATALOG_DIR), options.selection);
}

// Runs the selected workloads under the options' policy, printing each as it finishes, writes the result
// where the options ask, and returns 1 when a workload failed.
int run_catalog(const Options& options)
{
    const Policy                    policy  = Policy::resolve(*options.policy, options.overrides, hardware_threads());
    const std::vector<CatalogEntry> catalog = selected(options);
    // Opened before the run, so a path it cannot write fails at once.
    std::ofstream file;
    if (options.out) {
        file.open(std::filesystem::u8path(*options.out), std::ios::binary);
        if (!file)
            throw std::runtime_error("cannot write " + *options.out);
    }
    ReportOptions report;
    report.verbose        = options.verbose;
    report.collapse_below = options.collapse_below.value_or(report.collapse_below);
    report.sort_by        = options.sort_by.value_or(report.sort_by);
    report.color          = use_color(options.color, enable_terminal_escapes(), std::getenv("NO_COLOR"), std::getenv("TERM"));
    const std::unique_ptr<Reporter> reporter = make_reporter(options.reporter.value_or("console"), std::cout, report);
    Progress                        progress(std::cerr, error_is_terminal());
    SlicerEnvironment               environment(ORCABENCH_RESOURCES_DIR);
    const RunEvents                 events = report_events(*reporter, options.quiet ? nullptr : &progress);
    const Result                    result = run_suite(catalog, policy, WorkloadKinds::instance(), environment, host_reading, events,
                                                       options.dump_gcode.value_or(""));
    if (options.out) {
        file << write_document(result);
        file.close();
        if (!file)
            throw std::runtime_error("cannot write " + *options.out);
    }
    return count_of(result, Outcome::Failed) == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv)
{
    // Converts the arguments to UTF-8 on Windows, the encoding orca_bench reads every path in.
    const boost::nowide::args utf8(argc, argv);
    try {
        WorkloadKinds::instance().require_registered();
        const Options options = parse_options(program_arguments(argc, argv));
        if (options.help) {
            std::cout << usage();
            return 0;
        }
        if (options.compare)
            return compare_files(options);
        if (options.list) {
            std::cout << listing(selected(options));
            return 0;
        }
        if (options.policy)
            return run_catalog(options);
        std::cout << usage();
        return 0;
    } catch (const OptionsError& error) {
        std::cerr << "orca_bench: " << printable_lines(error.what()) << "\n" << usage();
        return 2;
    } catch (const PolicyError& error) {
        std::cerr << "orca_bench: " << printable_lines(error.what()) << "\n" << usage();
        return 2;
    } catch (const CompareError& error) {
        std::cerr << "orca_bench: " << printable_lines(error.what()) << "\npass --allow-mismatch to compare them anyway\n";
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "orca_bench: " << printable_lines(error.what()) << "\n";
        return 1;
    }
}
