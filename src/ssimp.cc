#include <iostream>
#include <fstream>
#include <iomanip>
#include <limits>
#include <cassert>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <iterator>
#include <gsl/gsl_statistics_int.h>
#include <gsl/gsl_vector.h>

#include "options.hh"
#include "file.reading.hh"

#include "other/utils.hh"
#include "other/mvn.hh"
#include "other/DIE.hh"
#include "other/PP.hh"
#include "other/range.hh"

using std:: cout;
using std:: endl;
using std:: string;
using std:: vector;
using std:: setw;
using std:: unique_ptr;
using std:: make_unique;
using std:: ofstream;
using std:: ostream;

using file_reading:: chrpos;
using file_reading:: SNPiterator;
using file_reading:: GenotypeFileHandle;
using file_reading:: GwasFileHandle;

using utils:: ssize;


namespace ssimp {
// Some forward declarations
static
void impute_all_the_regions( file_reading:: GenotypeFileHandle         raw_ref_file
                             , file_reading:: GwasFileHandle             gwas
                             );
static
std:: unordered_map<string, chrpos>
            map_rs_to_chrpos( file_reading:: GenotypeFileHandle raw_ref_file);
static
std:: unordered_multimap<chrpos, string>
            make_map_chrpos_to_SNPname( file_reading:: GenotypeFileHandle raw_ref_file);
static
vector<vector<int>>
lookup_many_ref_rows( vector<SNPiterator<GenotypeFileHandle>>   const &  snps
                , file_reading:: CacheOfRefPanelData              &  cache
                );
static
mvn:: SquareMatrix
make_C_tag_tag_matrix(
                    vector<vector<int>>              const & genotypes_for_the_tags
                    , double lambda
                    );
static
mvn:: Matrix make_c_unkn_tags_matrix
        ( vector<vector<int>>              const & genotypes_for_the_tags
        , vector<vector<int>>              const & genotypes_for_the_unks
        , vector<SNPiterator<GenotypeFileHandle>> const & tag_its
        , vector<SNPiterator<GenotypeFileHandle>> const & unk_its
        , double                                          lambda
        );

enum class which_direction_t { DIRECTION_SHOULD_BE_REVERSED
                             , NO_ALLELE_MATCH
                             , DIRECTION_AS_IS };
string to_string(which_direction_t dir) {
    switch(dir) {
        break; case which_direction_t:: DIRECTION_SHOULD_BE_REVERSED: return "DIRECTION_SHOULD_BE_REVERSED";
        break; case which_direction_t:: NO_ALLELE_MATCH:              return "NO_ALLELE_MATCH";
        break; case which_direction_t:: DIRECTION_AS_IS:              return "DIRECTION_AS_IS";
    }
}
static
    which_direction_t decide_on_a_direction
    ( SNPiterator<GenotypeFileHandle> const & //r
    , SNPiterator<GwasFileHandle>     const & //g
    );
} // namespace ssimp

int main(int argc, char **argv) {

    // all options now read. Start checking they are all present
    options:: read_in_all_command_line_options(argc, argv);

    cout.imbue(std::locale("")); // apply the user's locale, for example the thousands separator
    cout << std::setprecision(20);

    if( options:: opt_raw_ref.empty() ||  options:: opt_gwas_filename.empty()) {
        DIE("Should pass args.\n    Usage:   " + string(argv[0]) + " --ref REFERENCEVCF --gwas GWAS --lambda 0.0 --window.width 1000000 --flanking.width 250000");
    }

    if(!options:: opt_raw_ref.empty() && !options:: opt_gwas_filename.empty()) {
        PP( options:: opt_raw_ref
          , options:: opt_gwas_filename
          , options:: opt_window_width
          );

        // Load the two files
        auto raw_ref_file = file_reading:: read_in_a_raw_ref_file(options:: opt_raw_ref);
        auto gwas         = file_reading:: read_in_a_gwas_file(options:: opt_gwas_filename);

        // Compare the chrpos in both files.
        // If GWAS has chrpos, it should be the same as in the RefPanel.
        // If GWAS doesn't have chrpos, copy it from the ref panel
        auto m            = ssimp:: map_rs_to_chrpos( raw_ref_file );
        update_positions_by_comparing_to_another_set( gwas, m );

        // The RefPanel has already been automatically sorted by chrpos,
        // but now we must do it for the GWAS, as the positions might have
        // changed in the GWAS.
        gwas->sort_my_entries();

        // Count how many GWAS SNPs have no position, and therefore
        // will be ignored.
        auto number_of_GWASsnps_with_unknown_position = std:: count_if(
                begin_from_file(gwas)
               ,  end_from_file(gwas)
               , [](auto v) { return v == chrpos{-1,-1}; }
               );

        // Print the various SNP counts
        cout << '\n';
        PP(raw_ref_file->number_of_snps());
        PP(        gwas->number_of_snps());
        PP(number_of_GWASsnps_with_unknown_position);

        cout << '\n';
        // Go through regions, printing how many
        // SNPs there are in each region
        ssimp:: impute_all_the_regions(raw_ref_file, gwas);
    }
}

namespace ssimp{

using file_reading:: GenotypeFileHandle;
using file_reading:: GwasFileHandle;

static
void impute_all_the_regions( file_reading:: GenotypeFileHandle         ref_panel
                             , file_reading:: GwasFileHandle             gwas
                             ) {
    unique_ptr<ofstream>   out_stream_for_imputations;
    ostream              * out_stream_ptr = nullptr;
    if(!options:: opt_out.empty()) {
        out_stream_for_imputations = make_unique<ofstream>( options:: opt_out. c_str() );
        (*out_stream_for_imputations) || DIE("Couldn't open the --out file [" + options:: opt_out + "]");
        out_stream_ptr = &*out_stream_for_imputations;
    }
    else {
        // If --out isn't specified, just print to stdout
        out_stream_ptr = & cout;
    }
    assert(out_stream_ptr);
    auto const b_ref  = begin_from_file(ref_panel);
    auto const e_ref  =   end_from_file(ref_panel);
    auto const b_gwas = begin_from_file(gwas);
    auto const e_gwas =   end_from_file(gwas);

    PP(options:: opt_window_width
      ,options:: opt_flanking_width
            );

    auto const map_chrpos_to_SNPname           = ssimp:: make_map_chrpos_to_SNPname( ref_panel );

    for(int chrm =  1; chrm <= 22; ++chrm) {
        file_reading:: CacheOfRefPanelData cache(ref_panel);;

        // First, find the begin and end of this chromosome
        auto c_begin = std:: lower_bound(b_ref, e_ref, chrpos{chrm, std::numeric_limits<int>::lowest() });
        auto c_end   = std:: lower_bound(b_ref, e_ref, chrpos{chrm, std::numeric_limits<int>::max()  });
        assert(c_end >= c_begin);
        if(c_begin != c_end)
            assert(c_begin.get_chrpos().pos >= 0); // first position is at least zero

        for(int w = 0; ; ++w ) {
            int current_window_start = w     * options:: opt_window_width;
            int current_window_end   = (w+1) * options:: opt_window_width;
            auto w_ref_narrow_begin = std:: lower_bound(c_begin, c_end, chrpos{chrm,current_window_start});
            auto w_ref_narrow_end   = std:: lower_bound(c_begin, c_end, chrpos{chrm,current_window_end  });
            if(w_ref_narrow_begin == c_end)
                break; // Finished with this chromosome
            if(w_ref_narrow_begin == w_ref_narrow_end)
                continue; // Nothing to impute, skip to the next region

            auto w_ref_wide_begin = std:: lower_bound(c_begin, c_end, chrpos{chrm,current_window_start - options:: opt_flanking_width});
            auto w_ref_wide_end   = std:: lower_bound(c_begin, c_end, chrpos{chrm,current_window_end   + options:: opt_flanking_width });

            // Look up this region in the GWAS (taking account of the flanking width also)
            auto w_gwas_begin = std:: lower_bound(b_gwas, e_gwas, chrpos{chrm,current_window_start - options:: opt_flanking_width});
            auto w_gwas_end   = std:: lower_bound(b_gwas, e_gwas, chrpos{chrm,current_window_end   + options:: opt_flanking_width});

            // New, more flexible, method for finding tags
            vector<double>                          tag_zs;
            vector<SNPiterator<GenotypeFileHandle>> tag_its;
            for(auto tag_candidate = range:: range_from_begin_end( w_gwas_begin, w_gwas_end )
                    ; ! tag_candidate.empty()
                    ;   tag_candidate.advance()
                    ) {
                auto crps = tag_candidate.current_it().get_chrpos();
                // Find the ref panel entries in the same range
                auto ref_candidates = range:: range_from_begin_end(
                         std:: lower_bound( w_ref_wide_begin , w_ref_wide_end, crps )
                        ,std:: upper_bound( w_ref_wide_begin , w_ref_wide_end, crps )
                        );
                vector<double> one_tag_zs;
                vector<SNPiterator<GenotypeFileHandle>> one_tag_its;
                for(; ! ref_candidates.empty(); ref_candidates.advance()) {
                    assert( ref_candidates.current_it().get_chrpos() == crps );
                    auto dir = decide_on_a_direction( ref_candidates.current_it()
                                         , tag_candidate .current_it() );
                    switch(dir) {
                        break; case which_direction_t:: DIRECTION_SHOULD_BE_REVERSED:
                            one_tag_zs.push_back( -tag_candidate.current_it().get_z() );
                            one_tag_its.push_back( ref_candidates.current_it()        );
                        break; case which_direction_t:: DIRECTION_AS_IS             :
                            one_tag_zs.push_back(  tag_candidate.current_it().get_z() );
                            one_tag_its.push_back( ref_candidates.current_it()        );
                        break; case which_direction_t:: NO_ALLELE_MATCH             : ;
                    }
                }
                assert(one_tag_zs.size() == one_tag_its.size());
                assert(one_tag_zs.size() <= 1);
                if(1==one_tag_zs.size()) {
                    for(auto z : one_tag_zs)
                        tag_zs.push_back(z);
                    for(auto it : one_tag_its)
                        tag_its.push_back(it);
                }
            }
            assert(tag_zs.size() == tag_its.size());

            int const number_of_tags = tag_zs.size();
            if(number_of_tags == 0)
                continue;

            // Now, find suitable targets - i.e. anything in the reference panel in the narrow window
            vector<chrpos>  SNPs_all_targets;
            vector<SNPiterator<GenotypeFileHandle>> unk_its;
            for(auto it = w_ref_narrow_begin; it<w_ref_narrow_end; ++it) {
                // actually, we should think about ignoring SNPs in certain situations

                auto const & z12_for_this_SNP = cache.lookup_one_ref_get_calls(it);
                if (z12_for_this_SNP.empty()) {
                    // empty vector means the SNP is not binary in the reference panel
                    continue;
                }
                auto z12_minmax = minmax_element(z12_for_this_SNP.begin(), z12_for_this_SNP.end());
                if  (*z12_minmax.first == *z12_minmax.second){
                    continue; // no variation in this SNP within the ref panel, therefore useless for imputation
                }
                SNPs_all_targets.push_back( it.get_chrpos() );
                unk_its         .push_back( it              );
            }


            // We have at least one SNP here, so let's print some numbers about this region
            auto number_of_snps_in_the_gwas_in_this_region      = w_gwas_end - w_gwas_begin;
            int  number_of_all_targets                          = unk_its.size();

            cout
                << '\n'
                << "chrm" << chrm
                << "\t   " << current_window_start << '-' << current_window_end
                << '\n';

            cout << setw(8) << number_of_snps_in_the_gwas_in_this_region      << " # GWAS     SNPs in this window (with "<<options:: opt_flanking_width<<" flanking)\n";
            cout << setw(8) << number_of_tags                                 << " # SNPs in both (i.e. useful as tags)\n";
            cout << setw(8) << number_of_all_targets                          << " # target SNPs (anything in narrow window, will include some tags)\n";

            // Next, copy the calls in from the reference panel, ready for computation of correlation
            vector<vector<int>> genotypes_for_the_tags = lookup_many_ref_rows(tag_its, cache);
            vector<vector<int>> genotypes_for_the_unks = lookup_many_ref_rows(unk_its, cache);


            assert(number_of_tags == utils:: ssize(genotypes_for_the_tags));
            assert(number_of_all_targets == utils:: ssize(genotypes_for_the_unks));

            static_assert( std:: is_same< vector<vector<int>> , decltype(genotypes_for_the_tags) >{} ,""); // ints, not doubles, hence gsl_stats_int_correlation
            static_assert( std:: is_same< vector<vector<int>> , decltype(genotypes_for_the_unks) >{} ,""); // ints, not doubles, hence gsl_stats_int_correlation

            int const N_ref = genotypes_for_the_tags.at(0).size();
            assert(N_ref > 0);

            mvn:: SquareMatrix C = make_C_tag_tag_matrix(genotypes_for_the_tags, options:: opt_lambda);
            mvn:: VecCol C_inv_zs    = solve_a_matrix (C, mvn:: make_VecCol(tag_zs));
            mvn:: Matrix      c = make_c_unkn_tags_matrix( genotypes_for_the_tags
                                                         , genotypes_for_the_unks
                                                         , tag_its
                                                         , unk_its
                                                         , options:: opt_lambda
                                                         );

            auto c_Cinv_zs = mvn:: multiply_matrix_by_colvec_giving_colvec(c, C_inv_zs);

#if 0
            { // verify that this gets the same results
                auto cCzs = multiply_matrix_by_colvec_giving_colvec
                            ( c
                            , multiply_matrix_by_colvec_giving_colvec
                              ( invert_a_matrix(C)
                              , mvn:: make_VecCol(tag_zs)
                              )
                            );
                assert(number_of_all_targets == ssize(cCzs));
                auto diff = c_Cinv_zs - cCzs;
                double mn, mx;
                gsl_vector_minmax(diff.get(), &mn, &mx);
                assert(mn > -1e-10);
                assert(mx <  1e-10);
            }
#endif


            assert(number_of_all_targets == ssize(c_Cinv_zs));
            for(int i=0; i<number_of_all_targets; ++i) {
                auto crps = SNPs_all_targets.at(i);
                auto it = map_chrpos_to_SNPname.find(crps);
                assert(it != map_chrpos_to_SNPname.end());
                while (it != map_chrpos_to_SNPname.end() && it->first == crps) {
                    auto SNPname = it->second;
                    (*out_stream_ptr)
                        << crps
                        << '\t' << c_Cinv_zs(i)
                        << '\t' << SNPname
                        << endl;
                    ++it;
                }
            }
        }
    }
}

static
std:: unordered_map<string, chrpos>
            map_rs_to_chrpos( file_reading:: GenotypeFileHandle raw_ref_file ) {
    auto       b = begin_from_file(raw_ref_file);
    auto const e =   end_from_file(raw_ref_file);
    std:: unordered_map<string, chrpos> m;
    for(;b<e; ++b) {
        auto rel = m.insert( std:: make_pair(b.get_SNPname(), b.get_chrpos()) );
        rel.second || DIE("same SNPname twice in the ref panel [" << b.get_SNPname() << "]");
    }
    return m;
}
static
std:: unordered_multimap<chrpos, string>
            make_map_chrpos_to_SNPname( file_reading:: GenotypeFileHandle raw_ref_file ) {
    auto       b = begin_from_file(raw_ref_file);
    auto const e =   end_from_file(raw_ref_file);
    std:: unordered_multimap<chrpos, string> m;
    for(;b<e; ++b) {
        m.insert( std:: make_pair(b.get_chrpos(), b.get_SNPname()) );
    }
    return m;
}
static
vector<vector<int>>
lookup_many_ref_rows( vector<SNPiterator<GenotypeFileHandle>>   const &  snps
                , file_reading:: CacheOfRefPanelData              &  cache
                ) {
    vector<vector<int>> many_calls;
    for(auto crps : snps) {
        many_calls.push_back( cache.lookup_one_ref_get_calls(crps));
    }
    assert(many_calls.size() == snps.size());
    return many_calls;
}

static
mvn:: SquareMatrix
make_C_tag_tag_matrix( vector<vector<int>>              const & genotypes_for_the_tags
                     , double                                   lambda
                     ) {
    int const number_of_tags = genotypes_for_the_tags.size();
    int const N_ref = genotypes_for_the_tags.at(0).size();
    assert(N_ref > 0);

    mvn:: SquareMatrix C (number_of_tags);
    for(int k=0; k<number_of_tags; ++k) {
        for(int l=0; l<number_of_tags; ++l) {
            assert(N_ref        == utils:: ssize(genotypes_for_the_tags.at(k)));
            assert(N_ref        == utils:: ssize(genotypes_for_the_tags.at(l)));
            double c_kl = gsl_stats_int_correlation( &genotypes_for_the_tags.at(k).front(), 1
                                                   , &genotypes_for_the_tags.at(l).front(), 1
                                                   , N_ref );
            if(c_kl > 1.0) { // sometimes it sneaks above one, don't really know how
                assert(c_kl-1.0 < 1e-5);
                c_kl = 1.0;
            }
            assert(c_kl >= -1.0);
            assert(c_kl <=  1.0);
            if(k==l) {
                assert(c_kl == 1.0);
                C.set(k,l,c_kl);
            }
            else {
                assert(c_kl <  1.0);
                C.set(k,l,c_kl * (1.0-lambda));
            }
        }
    }
    return C;
}
static
mvn:: Matrix make_c_unkn_tags_matrix
        ( vector<vector<int>>              const & genotypes_for_the_tags
        , vector<vector<int>>              const & genotypes_for_the_unks
        , vector<SNPiterator<GenotypeFileHandle>> const & tag_its
        , vector<SNPiterator<GenotypeFileHandle>> const & unk_its
        , double                                          lambda
        ) {
    int const number_of_tags = genotypes_for_the_tags.size();
    int const number_of_all_targets = genotypes_for_the_unks.size();
    assert(number_of_tags        == ssize(tag_its));
    assert(number_of_all_targets == ssize(unk_its));
    int const N_ref = genotypes_for_the_tags.at(0).size();
    assert(N_ref > 0);

    mvn:: Matrix      c(number_of_all_targets, number_of_tags);

    for(auto tags : zip_val( range:: ints(number_of_tags)
                           , range:: range_from_begin_end(tag_its)
                )) {
        int  k          = std::get<0>(tags);
        auto tag_its_k  = std::get<1>(tags);
        assert(tag_its_k == tag_its.at(k));
        for(int u : range:: ints(number_of_all_targets)) {
            assert(N_ref        == utils:: ssize(genotypes_for_the_tags.at(k)));
            assert(N_ref        == utils:: ssize(genotypes_for_the_unks.at(u)));
            double c_ku = gsl_stats_int_correlation( &genotypes_for_the_tags.at(k).front(), 1
                                                   , &genotypes_for_the_unks.at(u).front(), 1
                                                   , N_ref );
            if(c_ku > 1.0) {
                assert(c_ku-1.0 < 1e-5);
                c_ku = 1.0;
            }

            if(tag_its_k.m_line_number == unk_its.at(u).m_line_number) {
                assert(c_ku ==  1.0);
                c.set(u,k,c_ku);
            }
            else {
                assert(c_ku >= -1.0);
                assert(c_ku <=  1.0);
                c.set(u,k,c_ku * (1.0-lambda));
            }
        }
    }
    return c;
};
static
    which_direction_t decide_on_a_direction
    ( SNPiterator<GenotypeFileHandle> const & r
    , SNPiterator<GwasFileHandle>     const & g
    ) {
        auto const rp_ref = r.get_allele_ref();
        auto const rp_alt = r.get_allele_alt();
        auto const gw_ref = g.get_allele_ref();
        auto const gw_alt = g.get_allele_alt();

        //PP(r.get_chrpos() ,rp_ref ,rp_alt ,gw_ref ,gw_alt);
        assert(rp_ref != rp_alt);
        assert(gw_ref != gw_alt);

        if( rp_ref == gw_ref
         && rp_alt == gw_alt)
            return which_direction_t:: DIRECTION_AS_IS;

        if( rp_ref == gw_alt
         && rp_alt == gw_ref)
            return which_direction_t:: DIRECTION_SHOULD_BE_REVERSED;

        return which_direction_t:: NO_ALLELE_MATCH;
    }

} // namespace ssimp