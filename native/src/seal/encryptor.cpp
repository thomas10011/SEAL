// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT license.

#include <algorithm>
#include <stdexcept>
#include "seal/encryptor.h"
#include "seal/randomgen.h"
#include "seal/randomtostd.h"
#include "seal/smallmodulus.h"
#include "seal/util/common.h"
#include "seal/util/uintcore.h"
#include "seal/util/uintarithmod.h"
#include "seal/util/uintarithsmallmod.h"
#include "seal/util/uintarith.h"
#include "seal/util/polyarithsmallmod.h"
#include "seal/util/clipnormal.h"
#include "seal/util/smallntt.h"
#include "seal/util/rlwe.h"
#include "seal/util/scalingvariant.h"

using namespace std;
using namespace seal::util;


namespace seal
{
    Encryptor::Encryptor(shared_ptr<SEALContext> context,
        const PublicKey &public_key) : context_(move(context))
    {
        // Verify parameters
        if (!context_)
        {
            throw invalid_argument("invalid context");
        }
        if (!context_->parameters_set())
        {
            throw invalid_argument("encryption parameters are not set correctly");
        }

        set_public_key(public_key);

        auto &parms = context_->key_context_data()->parms();
        auto &coeff_modulus = parms.coeff_modulus();
        size_t coeff_count = parms.poly_modulus_degree();
        size_t coeff_mod_count = coeff_modulus.size();

        // Quick sanity check
        if (!util::product_fits_in(coeff_count, coeff_mod_count, size_t(2)))
        {
            throw logic_error("invalid parameters");
        }
    }

    Encryptor::Encryptor(shared_ptr<SEALContext> context,
        const SecretKey &secret_key) : context_(move(context))
    {
        // Verify parameters
        if (!context_)
        {
            throw invalid_argument("invalid context");
        }
        if (!context_->parameters_set())
        {
            throw invalid_argument("encryption parameters are not set correctly");
        }

        set_secret_key(secret_key);

        auto &parms = context_->key_context_data()->parms();
        auto &coeff_modulus = parms.coeff_modulus();
        size_t coeff_count = parms.poly_modulus_degree();
        size_t coeff_mod_count = coeff_modulus.size();

        // Quick sanity check
        if (!util::product_fits_in(coeff_count, coeff_mod_count, size_t(2)))
        {
            throw logic_error("invalid parameters");
        }
    }

    Encryptor::Encryptor(shared_ptr<SEALContext> context,
        const PublicKey &public_key, const SecretKey &secret_key)
        : context_(move(context))
    {
        // Verify parameters
        if (!context_)
        {
            throw invalid_argument("invalid context");
        }
        if (!context_->parameters_set())
        {
            throw invalid_argument("encryption parameters are not set correctly");
        }

        set_public_key(public_key);
        set_secret_key(secret_key);

        auto &parms = context_->key_context_data()->parms();
        auto &coeff_modulus = parms.coeff_modulus();
        size_t coeff_count = parms.poly_modulus_degree();
        size_t coeff_mod_count = coeff_modulus.size();

        // Quick sanity check
        if (!util::product_fits_in(coeff_count, coeff_mod_count, size_t(2)))
        {
            throw logic_error("invalid parameters");
        }
    }

    void Encryptor::encrypt_zero_custom(parms_id_type parms_id, Ciphertext &destination,
        bool is_asymmetric, bool save_seed, MemoryPoolHandle pool) const
    {
        // Verify parameters.
        if (!pool)
        {
            throw invalid_argument("pool is uninitialized");
        }

        auto context_data_ptr = context_->get_context_data(parms_id);
        if (!context_data_ptr)
        {
            throw invalid_argument("parms_id is not valid for encryption parameters");
        }

        auto &context_data = *context_->get_context_data(parms_id);
        auto &parms = context_data.parms();
        size_t coeff_mod_count = parms.coeff_modulus().size();
        size_t coeff_count = parms.poly_modulus_degree();
        bool is_ntt_form = false;

        if (parms.scheme() == scheme_type::CKKS)
        {
            is_ntt_form = true;
        }
        else if (parms.scheme() != scheme_type::BFV)
        {
            throw invalid_argument("unsupported scheme");
        }

        // Resize destination and save results
        destination.resize(context_, parms_id, 2);

        // If asymmetric key encryption
        if (is_asymmetric)
        {
            auto prev_context_data_ptr = context_data.prev_context_data();
            if (prev_context_data_ptr)
            {
                // Requires modulus switching
                auto &prev_context_data = *prev_context_data_ptr;
                auto &prev_parms_id = prev_context_data.parms_id();
                auto &base_converter = prev_context_data.base_converter();

                // Zero encryption without modulus switching
                Ciphertext temp(pool);
                util::encrypt_zero_asymmetric(public_key_, context_, prev_parms_id,
                    is_ntt_form, temp);

                // Modulus switching
                for (size_t j = 0; j < 2; j++)
                {
                    if (is_ntt_form)
                    {
                        base_converter->round_last_coeff_modulus_ntt_inplace(
                            temp.data(j),
                            prev_context_data.small_ntt_tables(),
                            pool);
                    }
                    else
                    {
                        base_converter->round_last_coeff_modulus_inplace(
                            temp.data(j),
                            pool);
                    }
                    util::set_poly_poly(
                        temp.data(j),
                        coeff_count,
                        coeff_mod_count,
                        destination.data(j));
                }
                destination.is_ntt_form() = is_ntt_form;
                destination.scale() = temp.scale();
                destination.parms_id() = parms_id;
            }
            else
            {
                // Does not require modulus switching
                util::encrypt_zero_asymmetric(public_key_, context_, parms_id,
                    is_ntt_form, destination);
            }
        }
        else
        {
            util::encrypt_zero_symmetric(secret_key_, context_, parms_id,
                is_ntt_form, save_seed, destination);
            // Does not require modulus switching
        }
    }

    void Encryptor::encrypt_custom(const Plaintext &plain, Ciphertext &destination,
        bool is_asymmetric, bool save_seed, MemoryPoolHandle pool) const
    {
        // Verify that plain is valid.
        if (!is_valid_for(plain, context_))
        {
            throw invalid_argument("plain is not valid for encryption parameters");
        }

        auto scheme = context_->key_context_data()->parms().scheme();
        if (scheme == scheme_type::BFV)
        {
            if (plain.is_ntt_form())
            {
                throw invalid_argument("plain cannot be in NTT form");
            }

            encrypt_zero_custom(context_->first_parms_id(), destination,
                is_asymmetric, save_seed, pool);

            // Multiply plain by scalar coeff_div_plaintext and reposition if in upper-half.
            // Result gets added into the c_0 term of ciphertext (c_0,c_1).
            util::multiply_add_plain_with_scaling_variant(
                plain, *context_->first_context_data(), destination.data());
        }
        else if (scheme == scheme_type::CKKS)
        {
            if (!plain.is_ntt_form())
            {
                throw invalid_argument("plain must be in NTT form");
            }

            auto context_data_ptr = context_->get_context_data(plain.parms_id());
            if (!context_data_ptr)
            {
                throw invalid_argument("plain is not valid for encryption parameters");
            }
            encrypt_zero_custom(plain.parms_id(), destination,
                is_asymmetric, save_seed, pool);

            auto &parms = context_->get_context_data(plain.parms_id())->parms();
            auto &coeff_modulus = parms.coeff_modulus();
            size_t coeff_mod_count = coeff_modulus.size();
            size_t coeff_count = parms.poly_modulus_degree();

            // The plaintext gets added into the c_0 term of ciphertext (c_0,c_1).
            for (size_t i = 0; i < coeff_mod_count; i++)
            {
                util::add_poly_poly_coeffmod(
                    destination.data() + (i * coeff_count),
                    plain.data() + (i * coeff_count),
                    coeff_count,
                    coeff_modulus[i],
                    destination.data() + (i * coeff_count));
            }

            destination.scale() = plain.scale();
        }
        else
        {
            throw invalid_argument("unsupported scheme");
        }
    }

    void Encryptor::compose_single_coeff(
        const SEALContext::ContextData& context_data, uint64_t* value) {

        auto& parms = context_data.parms();
        auto& coeff_modulus = parms.coeff_modulus();
        size_t coeff_count = parms.poly_modulus_degree();
        size_t coeff_mod_count = coeff_modulus.size();

        auto& base_converter = context_data.base_converter();
        auto coeff_products_array = base_converter->get_coeff_products_array();
        auto& inv_coeff_mod_coeff_array = base_converter->get_inv_coeff_mod_coeff_array();

        auto copy(allocate_uint(coeff_mod_count, MemoryPoolHandle::Global()));
        for (size_t j = 0; j < coeff_mod_count; j++)
        {
            copy[j] = value[j];
        }


        auto temp(allocate_uint(coeff_mod_count, MemoryPoolHandle::Global()));
        set_zero_uint(coeff_mod_count, value);

        for (size_t j = 0; j < coeff_mod_count; j++)
        {
            uint64_t tmp = multiply_uint_uint_mod(copy.get()[j],
                inv_coeff_mod_coeff_array[j], coeff_modulus[j]);
            multiply_uint_uint64(coeff_products_array + (j * coeff_mod_count),
                coeff_mod_count, tmp, coeff_mod_count, temp.get());
            add_uint_uint_mod(temp.get(), value,
                context_data.total_coeff_modulus(),
                coeff_mod_count, value);
        }
        set_zero_uint(coeff_mod_count, temp.get());
    }



}
