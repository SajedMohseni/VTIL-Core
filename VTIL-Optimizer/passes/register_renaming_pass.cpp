// Copyright (c) 2020 Can Boluk and contributors of the VTIL Project   
// All rights reserved.   
//    
// Redistribution and use in source and binary forms, with or without   
// modification, are permitted provided that the following conditions are met: 
//    
// 1. Redistributions of source code must retain the above copyright notice,   
//    this list of conditions and the following disclaimer.   
// 2. Redistributions in binary form must reproduce the above copyright   
//    notice, this list of conditions and the following disclaimer in the   
//    documentation and/or other materials provided with the distribution.   
// 3. Neither the name of mosquitto nor the names of its   
//    contributors may be used to endorse or promote products derived from   
//    this software without specific prior written permission.   
//    
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" 
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE   
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE  
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE   
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR   
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF   
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS   
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN   
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)   
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE  
// POSSIBILITY OF SUCH DAMAGE.        
//
#include "register_renaming_pass.hpp"
#include <vtil/symex>
#include <algorithm>
#include <vtil/query>
#include "../common/auxiliaries.hpp"

namespace vtil::optimizer
{
	// Implement the pass.
	//
	size_t register_renaming_pass::pass( basic_block* blk, bool xblock )
	{
		size_t cnt = 0;
		cached_tracer tracer = {};

		// For each instruction:
		//
		for ( auto it = blk->begin(); !it.is_end(); it++ )
		{
			// If not [mov regN regN / movsx regN regN], skip.
			//
			if ( ( *it->base != ins::mov && *it->base != ins::movsx ) ||
				 !it->operands[ 0 ].is_register() || !it->operands[ 1 ].is_register() ||
				 it->operands[ 0 ].reg().bit_count != it->operands[ 1 ].reg().bit_count )
				continue;
			
			// Skip if non-position bound.
			//
			symbolic::variable dst = { it, it->operands[ 0 ].reg() };
			symbolic::variable src = { it, it->operands[ 1 ].reg() };
			if ( !src.at.is_valid() || !dst.at.is_valid() || dst.reg().is_stack_pointer() )
				continue;

			// If src is used after this point, skip
			//
			if ( aux::is_used( src, xblock, &tracer ) )
				continue;

			// Path restrict iterator if not cross-block.
			//
			it.paths_allowed = {};
			it.is_path_restricted = !xblock;

			// Allocate fail and value mask.
			//
			bool fail = false;
			uint64_t mask = math::fill( it->operands[ 1 ].reg().bit_count );

			// => Begin backwards recursive query:
			//
			auto res = query::create_recursive( it, -1 )
				// @ Bind the mask to recursive path.
				.bind( mask )
				// := Unproject to iterator form.
				.unproject()
				// @ If destination is used by the instruction, fail.
				.until( [ & ] ( const il_iterator& i ) { return fail |= ( bool ) dst.read_by( i, &tracer ); })
				// | Filter to instructions that write to source.
				.where( [ & ] ( const il_iterator& i ) 
				{
					// If it does not write to source, skip.
					//
					auto details = src.accessed_by( i, &tracer );
					if ( !details || !details.write ) return false;

					// If out-of-bounds access, fail.
					//
					if ( details.bit_offset < 0 || ( details.bit_count + details.bit_offset ) > it->operands[ 1 ].reg().bit_count )
					{
						fail = true;
						return false;
					}

					// If source is not being read, clear the mask.
					//
					if( !details.read )
						query::rlocal( mask ) &= ~math::fill( details.bit_count, details.bit_offset );
					
					// Include in iteration.
					//
					return true;
				} )
				// >> Skip until mask is cleared.
				.where( [ & ] ( const il_iterator& it )
				{
					return query::rlocal( mask ) == 0;
				})
				.first();

			// If query did not fail and we have as many results as we expected:
			//
			if ( !fail && res.count_paths() == res.flatten( true ).result.size() )
			{
				// For each edge node:
				//
				for ( auto& it_begin : res.result )
				{
					// Restrict iterator paths.
					//
					it_begin.restrict_path( it.container, true );

					// => Begin forwards recursive query:
					//
					query::create_recursive( it_begin, +1 )
						// >> Iterate until the origin instruction is hit.
						.until( it )
						// @ For each instruction:
						.for_each( [ & ] ( instruction& ins )
						{
							// Iterate each operand:
							//
							for ( auto& op : ins.operands )
							{
								// Skip if not register or not overlapping with source.
								//
								if ( !op.is_register() ) continue;
								auto& reg = op.reg();
								if ( !reg.overlaps( src.reg() ) ) continue;

								// Swap with destination register.
								//
								reg.local_id = dst.reg().local_id;
								reg.flags = dst.reg().flags;
								reg.bit_offset += dst.reg().bit_offset - src.reg().bit_offset;
							}
						} );

					// Nop the origin instruction.
					//
					it->base = &ins::nop;
					it->operands = {};
					cnt++;
					tracer.flush();
				}
			}
		}

		// Compress register space!
		//
		return cnt;
	}
};